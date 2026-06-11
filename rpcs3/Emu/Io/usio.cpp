// v406 USIO emulator

#include "stdafx.h"
#include "usio.h"
#include "Input/pad_thread.h"
#include "Emu/Io/usio_config.h"
#include "Emu/IdManager.h"
#include "Emu/System.h"
#ifdef HAVE_LIBEVDEV
#include "Emu/Io/evdev_gun_handler.h"
#endif

#include <chrono>

LOG_CHANNEL(usio_log, "USIO");

namespace
{
	// Namco System 357 lightgun games read the raw JVS control word (handled
	// by translate_input_gun); Tekken/Taiko use a different bit/board layout
	// (translate_input_tekken/taiko, the upstream behaviour). These dumps all
	// share the placeholder serial SCEEXE000, so the game is identified by its
	// title. Extend this list as needed.
	// Lowercase the title and strip everything but letters/digits, so e.g.
	// "DarkEscape", "Dark Escape" and "DARK ESCAPE 4D" all normalize the same.
	std::string usio_normalized_title()
	{
		std::string out;
		for (const char c : Emu.GetTitle())
		{
			const unsigned char uc = static_cast<unsigned char>(c);
			if (std::isalnum(uc))
				out += static_cast<char>(std::tolower(uc));
		}
		return out;
	}

	bool usio_title_is_gun_game()
	{
		const std::string title = usio_normalized_title();

		static constexpr std::string_view gun_games[] =
		{
			"deadstorm",
			"razingstorm",
			"sailorzombie",
			"darkescape",
		};

		for (const std::string_view game : gun_games)
		{
			if (title.find(game) != umax)
				return true;
		}

		return false;
	}

	bool usio_title_is_razingstorm()
	{
		return usio_normalized_title().find("razingstorm") != umax;
	}

	// Tekken 6 / Tekken 6 Bloodline Rebellion use a different JVS switch bit
	// layout (16-bit player shift, standard byte0/byte1 order) than Tekken Tag
	// Tournament 2, which translate_input_tekken otherwise targets.
	bool usio_title_is_tekken6()
	{
		return usio_normalized_title().find("tekken6") != umax;
	}
}

#ifdef HAVE_LIBEVDEV
// Linux lightgun support via evdev. A single handler is shared through g_fxo
// and is polled inline whenever the game reads gun coordinates or buttons.
struct usio_gun_handler
{
	evdev_gun_handler handler;
	usio_gun_handler() { handler.init(); }
};
#endif

template <>
void fmt_class_string<usio_btn>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](usio_btn value)
	{
		switch (value)
		{
		case usio_btn::test: return "Test";
		case usio_btn::coin: return "Coin";
		case usio_btn::service: return "Service";
		case usio_btn::enter: return "Enter/Start";
		case usio_btn::up: return "Up";
		case usio_btn::down: return "Down";
		case usio_btn::left: return "Left";
		case usio_btn::right: return "Right";
		case usio_btn::taiko_hit_side_left: return "Taiko Hit Side Left";
		case usio_btn::taiko_hit_side_right: return "Taiko Hit Side Right";
		case usio_btn::taiko_hit_center_left: return "Taiko Hit Center Left";
		case usio_btn::taiko_hit_center_right: return "Taiko Hit Center Right";
		case usio_btn::tekken_button1: return "Tekken Button 1";
		case usio_btn::tekken_button2: return "Tekken Button 2";
		case usio_btn::tekken_button3: return "Tekken Button 3";
		case usio_btn::tekken_button4: return "Tekken Button 4";
		case usio_btn::tekken_button5: return "Tekken Button 5";
		case usio_btn::card_tapping: return "Card Tapping";
		case usio_btn::count: return "Count";
		}

		return unknown;
	});
}

struct usio_memory
{
	std::vector<u8> backup_memory;
	std::array<std::array<u8, 0x40>, g_cfg_usio.players.size()> card_data{};

	usio_memory() = default;
	usio_memory(const usio_memory&) = delete;
	usio_memory& operator=(const usio_memory&) = delete;

	void init()
	{
		backup_memory.clear();
		backup_memory.resize(page_size * page_count);
	}

	static constexpr usz page_size = 0x10000;
	static constexpr usz page_count = 0x10;
};

usb_device_usio::usb_device_usio(const std::array<u8, 7>& location)
	: usb_device_emulated(location)
{
	// Initialize dependencies
	g_fxo->need<usio_memory>();
#ifdef HAVE_LIBEVDEV
	g_fxo->need<usio_gun_handler>();
#endif

	device = UsbDescriptorNode(USB_DESCRIPTOR_DEVICE,
		UsbDeviceDescriptor{
			.bcdUSB             = 0x0110,
			.bDeviceClass       = 0xff,
			.bDeviceSubClass    = 0x00,
			.bDeviceProtocol    = 0xff,
			.bMaxPacketSize0    = 0x8,
			.idVendor           = 0x0b9a,
			.idProduct          = 0x0910,
			.bcdDevice          = 0x0910,
			.iManufacturer      = 0x01,
			.iProduct           = 0x02,
			.iSerialNumber      = 0x00,
			.bNumConfigurations = 0x01});

	auto& config0 = device.add_node(UsbDescriptorNode(USB_DESCRIPTOR_CONFIG,
		UsbDeviceConfiguration{
			.wTotalLength        = 39,
			.bNumInterfaces      = 0x01,
			.bConfigurationValue = 0x01,
			.iConfiguration      = 0x00,
			.bmAttributes        = 0xc0,
			.bMaxPower           = 0x32 // ??? 100ma
		}));

	config0.add_node(UsbDescriptorNode(USB_DESCRIPTOR_INTERFACE,
		UsbDeviceInterface{
			.bInterfaceNumber   = 0x00,
			.bAlternateSetting  = 0x00,
			.bNumEndpoints      = 0x03,
			.bInterfaceClass    = 0x00,
			.bInterfaceSubClass = 0x00,
			.bInterfaceProtocol = 0x00,
			.iInterface         = 0x00}));

	config0.add_node(UsbDescriptorNode(USB_DESCRIPTOR_ENDPOINT,
		UsbDeviceEndpoint{
			.bEndpointAddress = 0x01,
			.bmAttributes     = 0x02,
			.wMaxPacketSize   = 0x0040,
			.bInterval        = 0x00}));

	config0.add_node(UsbDescriptorNode(USB_DESCRIPTOR_ENDPOINT,
		UsbDeviceEndpoint{
			.bEndpointAddress = 0x82,
			.bmAttributes     = 0x02,
			.wMaxPacketSize   = 0x0040,
			.bInterval        = 0x00}));

	config0.add_node(UsbDescriptorNode(USB_DESCRIPTOR_ENDPOINT,
		UsbDeviceEndpoint{
			.bEndpointAddress = 0x83,
			.bmAttributes     = 0x03,
			.wMaxPacketSize   = 0x0008,
			.bInterval        = 16}));

	m_is_gun_game = usio_title_is_gun_game();
	m_is_razingstorm = usio_title_is_razingstorm();
	m_is_tekken6 = usio_title_is_tekken6();
	usio_log.notice("USIO: input layout = %s (title='%s')", m_is_gun_game ? (m_is_razingstorm ? "gun (razing storm)" : "gun (raw JVS)") : (m_is_tekken6 ? "tekken6" : "tekken"), Emu.GetTitle());

	load_backup();
}

usb_device_usio::~usb_device_usio()
{
	save_backup();
}

std::shared_ptr<usb_device> usb_device_usio::make_instance(u32, const std::array<u8, 7>& location)
{
	return std::make_shared<usb_device_usio>(location);
}

u16 usb_device_usio::get_num_emu_devices()
{
	return 1;
}

void usb_device_usio::control_transfer(u8 bmRequestType, u8 bRequest, u16 wValue, u16 wIndex, u16 wLength, u32 buf_size, u8* buf, UsbTransfer* transfer)
{
	transfer->fake = true;

	// Control transfers are nearly instant
	//switch (bmRequestType)
	{
	//default:
		// Follow to default emulated handler
		usb_device_emulated::control_transfer(bmRequestType, bRequest, wValue, wIndex, wLength, buf_size, buf, transfer);
		//break;
	}
}

extern bool is_input_allowed();

void usb_device_usio::load_backup()
{
	usio_memory& memory = g_fxo->get<usio_memory>();
	memory.init();

	fs::file usio_backup_file;

	if (!usio_backup_file.open(usio_backup_path, fs::read))
	{
		usio_log.trace("Failed to load the USIO Backup file: %s", usio_backup_path);
		return;
	}

	const u64 file_size = memory.backup_memory.size();

	if (usio_backup_file.size() != file_size)
	{
		usio_log.trace("Invalid USIO Backup file detected: %s", usio_backup_path);
		return;
	}

	usio_backup_file.read(memory.backup_memory.data(), file_size);

	for (usz i = 0; i < memory.card_data.size(); i++)
	{
		if (fs::file usio_card_file;
			usio_card_file.open(fmt::format("%s/caches/usio_card_p%d.bin", rpcs3::utils::get_hdd1_dir(), i + 1), fs::read) &&
			usio_card_file.size() == memory.card_data[i].size())
		{
			usio_card_file.read(memory.card_data[i].data(), memory.card_data[i].size());
		}
	}
}

void usb_device_usio::save_backup()
{
	if (!is_used)
		return;

	fs::file usio_backup_file;

	if (!usio_backup_file.open(usio_backup_path, fs::create + fs::write + fs::lock))
	{
		usio_log.error("Failed to save the USIO Backup file: %s", usio_backup_path);
		return;
	}

	const u64 file_size = g_fxo->get<usio_memory>().backup_memory.size();

	usio_backup_file.write(g_fxo->get<usio_memory>().backup_memory.data(), file_size);
	usio_backup_file.trunc(file_size);
}

void usb_device_usio::translate_input_taiko()
{
	std::lock_guard lock(pad::g_pad_mutex);
	const auto handler = pad::get_pad_thread();

	std::vector<u8> input_buf(0x60);
	constexpr le_t<u16> c_hit = 0x1800;
	le_t<u16> digital_input = 0;

	const auto translate_from_pad = [&](usz pad_number, usz player)
	{
		const usz offset = player * 8ULL;
		auto& status = m_io_status[0];

		if (const auto& pad = ::at32(handler->GetPads(), pad_number); pad->is_connected() && !pad->is_copilot() && is_input_allowed())
		{
			const auto& cfg = ::at32(g_cfg_usio.players, pad_number);
			cfg->handle_input(pad, false, [&](usio_btn btn, pad_button /*pad_btn*/, u16 /*value*/, bool pressed, bool& /*abort*/)
			{
				switch (btn)
				{
				case usio_btn::test:
					if (player != 0) break;
					if (pressed && !status.test_key_pressed) // Solve the need to hold the Test key
						status.test_on = !status.test_on;
					status.test_key_pressed = pressed;
					break;
				case usio_btn::coin:
					if (player != 0) break;
					if (pressed && !status.coin_key_pressed) // Ensure only one coin is inserted each time the Coin key is pressed
						status.coin_counter++;
					status.coin_key_pressed = pressed;
					break;
				case usio_btn::service:
					if (player == 0 && pressed)
						digital_input |= 0x4000;
					break;
				case usio_btn::enter:
					if (player == 0 && pressed)
						digital_input |= 0x200;
					break;
				case usio_btn::up:
					if (player == 0 && pressed)
						digital_input |= 0x2000;
					break;
				case usio_btn::down:
					if (player == 0 && pressed)
						digital_input |= 0x1000;
					break;
				case usio_btn::taiko_hit_side_left:
					if (pressed)
						std::memcpy(input_buf.data() + 32 + offset, &c_hit, sizeof(u16));
					break;
				case usio_btn::taiko_hit_center_right:
					if (pressed)
						std::memcpy(input_buf.data() + 36 + offset, &c_hit, sizeof(u16));
					break;
				case usio_btn::taiko_hit_side_right:
					if (pressed)
						std::memcpy(input_buf.data() + 38 + offset, &c_hit, sizeof(u16));
					break;
				case usio_btn::taiko_hit_center_left:
					if (pressed)
						std::memcpy(input_buf.data() + 34 + offset, &c_hit, sizeof(u16));
					break;
				case usio_btn::card_tapping:
					if (pressed)
						tap_card(player);
					break;
				default:
					break;
				}
			});
		}

		if (player == 0 && status.test_on)
			digital_input |= 0x80;
	};

	for (usz i = 0; i < m_io_status.size(); i++)
		m_io_status[i].card_tapped = false;
	for (usz i = 0; i < g_cfg_usio.players.size(); i++)
		translate_from_pad(i, i);

	std::memcpy(input_buf.data(), &digital_input, sizeof(u16));
	std::memcpy(input_buf.data() + 16, &m_io_status[0].coin_counter, sizeof(u16));

	response = std::move(input_buf);
}

void usb_device_usio::translate_input_gun()
{
	std::vector<u8> input_buf(0x180);
	le_t<u64> digital_input[2]{};
	le_t<u16> digital_input_lm = 0;
	bool wheel_left = false;  // DeadStorm Pirates steering wheel (incremental encoder)
	bool wheel_right = false;

	std::lock_guard lock(pad::g_pad_mutex);
	const auto handler = pad::get_pad_thread();

	const auto translate_from_pad = [&](usz pad_number, usz player)
	{
		const bool p1 = (player % 2 == 0);
		auto& status = m_io_status[player / 2];
		auto& input = digital_input[0]; // System 357 games read both players from board 0's control word

		// System 357 gun games share menu/service/decide bits (byte #1 <<8), but
		// Razing Storm assigns the Start/Trigger/Pedal bits (byte #2 <<16)
		// differently from Dead Storm / Sailor Zombie / Dark Escape - verified
		// empirically on hardware-dumped titles.
		const bool rs = m_is_razingstorm;
		const u64 bit_start   = rs ? (p1 ? 0x800000ULL : 0x400000ULL) : (p1 ? 0x200000ULL : 0x40000ULL);  // P1/P2 Start
		const u64 bit_trig_l  = rs ? (p1 ? 0x200000ULL : 0x100000ULL) : (p1 ? 0x800000ULL : 0x100000ULL); // P1/P2 Trigger
		const u64 bit_trig_r  = rs ? (p1 ? 0x80000ULL  : 0x40000ULL)  : (p1 ? 0x400000ULL : 0x80000ULL);  // P1/P2 Trigger Right / pedal
		const u64 bit_service = 0x4000ULL;
		const u64 bit_menu_up = 0x2000ULL;
		const u64 bit_menu_dn = 0x1000ULL;
		const u64 bit_decide  = 0x200ULL;   // Enter / decide
		const u64 bit_switch  = 0x20000ULL; // Dark Escape 2D/3D switch

		const auto& pad = ::at32(handler->GetPads(), pad_number);
		if (pad->is_connected() && !pad->is_copilot() && is_input_allowed())
		{
			const auto& cfg = ::at32(g_cfg_usio.players, pad_number);
			cfg->handle_input(pad, false, [&](usio_btn btn, pad_button /*pad_btn*/, u16 /*value*/, bool pressed, bool& /*abort*/)
				{
					switch (btn)
					{
					case usio_btn::test:
						if (!p1)
							break;
						if (pressed && !status.test_key_pressed)
							status.test_on = !status.test_on;
						status.test_key_pressed = pressed;
						break;
					case usio_btn::coin:
						// No coin2 game exists, so either player's coin button inserts a coin.
						if (pressed && !status.coin_key_pressed)
							status.coin_counter++;
						status.coin_key_pressed = pressed;
						break;
					case usio_btn::service:
						if (pressed)
							input |= bit_service;
						break;
					case usio_btn::up:
						if (pressed)
							input |= bit_menu_up; // Menu Up
						break;
					case usio_btn::down:
						if (pressed)
							input |= bit_menu_dn; // Menu Down
						break;
					case usio_btn::left:
						if (pressed)
						{
							input |= bit_decide; // Enter / decide
							wheel_left = true; // Dead Storm: steer left (either player drives the shared wheel)
						}
						break;
					case usio_btn::right:
						if (pressed)
							wheel_right = true; // Dead Storm: steer right (either player drives the shared wheel)
						break;
					case usio_btn::enter:
						if (pressed)
							input |= bit_start; // Start
						break;
					case usio_btn::tekken_button1:
						if (pressed)
							input |= bit_trig_l; // Trigger (fire)
						break;
					case usio_btn::tekken_button2:
						if (pressed)
							input |= bit_trig_r; // Trigger Right
						break;
					case usio_btn::tekken_button3:
						if (pressed)
							input |= bit_switch; // Dark Escape 2D/3D switch
						break;
					default:
						break;
					}
				});
		}

		if (p1 && status.test_on)
		{
			input |= 0x80;
			digital_input_lm |= 0x1000;
		}
	};

	for (usz i = 0; i < g_cfg_usio.players.size(); i++)
		translate_from_pad(i, i);

#ifdef HAVE_LIBEVDEV
	// Lightgun trigger/buttons -> JVS control word (same per-game bits).
	{
		usio_gun_handler& gun = g_fxo->get<usio_gun_handler>();
		std::scoped_lock lock(gun.handler.mutex);

		const u32 num_guns = gun.handler.get_num_guns();
		for (u32 g = 0; g < num_guns && g < 2; g++)
		{
			gun.handler.poll(g);

			const bool p1 = (g == 0);
			auto& input = digital_input[0];

			// Per-game JVS bits, matching translate_from_pad above.
			const bool rs = m_is_razingstorm;
			const u64 bit_start  = rs ? (p1 ? 0x800000ULL : 0x400000ULL) : (p1 ? 0x200000ULL : 0x40000ULL);  // Start
			const u64 bit_trig_l = rs ? (p1 ? 0x200000ULL : 0x100000ULL) : (p1 ? 0x800000ULL : 0x100000ULL); // Trigger (fire)
			const u64 bit_trig_r = rs ? (p1 ? 0x80000ULL  : 0x40000ULL)  : (p1 ? 0x400000ULL : 0x80000ULL);  // Trigger Right / pedal

			// Gun button -> JVS control word (same mapping for all gun games):
			//   left = fire, right = pedal, middle = Start, btn_1 = coin,
			//   btn_5/6/7/8 = menu up/down/left/right.
			if (gun.handler.get_button(g, gun_button::btn_left))   // fire
				input |= bit_trig_l;
			if (gun.handler.get_button(g, gun_button::btn_right))  // trigger right / pedal
				input |= bit_trig_r;
			if (gun.handler.get_button(g, gun_button::btn_middle)) // Start
				input |= bit_start;
			if (gun.handler.get_button(g, gun_button::btn_5))      // menu up
				input |= 0x2000ULL;
			if (gun.handler.get_button(g, gun_button::btn_6))      // menu down
				input |= 0x1000ULL;
			if (gun.handler.get_button(g, gun_button::btn_7))      // left (Enter/decide; Dead Storm steer left)
			{
				input |= 0x200ULL;
				wheel_left = true; // either gun drives the shared wheel
			}
			if (gun.handler.get_button(g, gun_button::btn_8)) // right (Dead Storm steer right)
				wheel_right = true; // either gun drives the shared wheel
			if (g == 0) // btn_1 -> coin (own debounce so it doesn't clobber the pad coin state)
			{
				const bool coin_now = gun.handler.get_button(g, gun_button::btn_1) != 0;
				if (coin_now && !m_gun_coin_pressed)
					m_io_status[0].coin_counter++;
				m_gun_coin_pressed = coin_now;
			}

			// Gun aim -> 0x1000 analog channels (P1 @ 0xA0/0xA2, P2 @ 0xA4/0xA6),
			// the slot Dead Storm Pirates reads for the lightgun. 8-bit position
			// scaled to 16-bit (*257), matching the old TeknoParrot 0x1000 path.
			const int xmax = gun.handler.get_axis_x_max(g);
			const int ymax = gun.handler.get_axis_y_max(g);
			const u8 ax8 = xmax > 0 ? static_cast<u8>(std::clamp(gun.handler.get_axis_x(g) * 255 / xmax, 0, 255)) : 0;
			const u8 ay8 = ymax > 0 ? static_cast<u8>(std::clamp(gun.handler.get_axis_y(g) * 255 / ymax, 0, 255)) : 0;
			// Per TeknoParrot: JVS analog is 16-bit BIG-ENDIAN, read by the game
			// as SIGNED (screen center = 0). High byte (at 0xA0) is the signed
			// position: offset-binary 0..0xFF mapped to signed -128..+127 via
			// ^0x80; low byte carries fine detail. (Writing it little-endian or
			// unsigned makes the value flip sign at screen-center -> corners.)
			const u8 hx = static_cast<u8>(ax8 ^ 0x80);
			const u8 hy = static_cast<u8>(ay8 ^ 0x80);
			input_buf[0xA0 + g * 4 + 0] = hx;  // X high byte (signed position)
			input_buf[0xA0 + g * 4 + 1] = ax8; // X low byte (fine detail)
			input_buf[0xA2 + g * 4 + 0] = hy;  // Y high byte
			input_buf[0xA2 + g * 4 + 1] = ay8; // Y low byte
		}
	}
#endif

	for (usz i = 0; i < 2; i++)
	{
		// Board 0 -> 0x80 (digital) / 0x90 (coin), board 1 -> 0x100 / 0x110.
		// This matches the layout the games actually read (the same one the
		// old TeknoParrot 0x1000 path produced); the previous mapping had the
		// two boards swapped, so P1 input/coin landed where the game never
		// looked and coins silently did nothing.
		std::memcpy(input_buf.data() + 0x80 + i * 0x80, &digital_input[i], sizeof(u64));
		std::memcpy(input_buf.data() + 0x80 + i * 0x80 + 0x10, &m_io_status[i].coin_counter, sizeof(u16));
	}

	// DeadStorm Pirates steering wheel: a free-spinning rotary encoder at board 0
	// + 0x30. The game reads the signed delta between samples as ongoing rotation,
	// so holding Left/Right must keep the value moving and WRAP it (255 -> 0 ...)
	// rather than clamp - clamping makes the in-game wheel stick at the limit
	// even though the test-menu bar (which shows the raw byte) looks maxed.
	// Released: hold the current position (counter-steer to wind back).
	{
		const u64 now_ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
		double dt = m_wheel_time_ns ? (now_ns - m_wheel_time_ns) / 1e9 : 0.0;
		m_wheel_time_ns = now_ns;
		dt = std::min(dt, 0.05); // ignore long gaps (e.g. after pause)

		constexpr double rate = 100.0; // encoder counts per second while held

		if (wheel_left)
			m_wheel_pos -= rate * dt;
		else if (wheel_right)
			m_wheel_pos += rate * dt;

		// Wrap to [0, 256) so the encoder keeps turning past the ends.
		while (m_wheel_pos >= 256.0)
			m_wheel_pos -= 256.0;
		while (m_wheel_pos < 0.0)
			m_wheel_pos += 256.0;

		input_buf[0xB0] = static_cast<u8>(m_wheel_pos);
	}

	std::memcpy(input_buf.data(), &digital_input_lm, sizeof(u16));
	input_buf[2] = 0b00010000; // DIP switches, 8 in total

	response = std::move(input_buf);
}


void usb_device_usio::translate_input_tekken()
{
	std::lock_guard lock(pad::g_pad_mutex);
	const auto handler = pad::get_pad_thread();

	std::vector<u8> input_buf(0x180);
	le_t<u64> digital_input[2]{};
	le_t<u16> digital_input_lm = 0;

	const auto translate_from_pad = [&](usz pad_number, usz player)
	{
		const usz shift = (player % 2) * (m_is_tekken6 ? 16ULL : 24ULL);
		auto& status = m_io_status[player / 2];
		auto& input = digital_input[player / 2];

		if (const auto& pad = ::at32(handler->GetPads(), pad_number); pad->is_connected() && !pad->is_copilot() && is_input_allowed())
		{
			const auto& cfg = ::at32(g_cfg_usio.players, pad_number);
			cfg->handle_input(pad, false, [&](usio_btn btn, pad_button /*pad_btn*/, u16 /*value*/, bool pressed, bool& /*abort*/)
			{
				switch (btn)
				{
				case usio_btn::test:
					if (player % 2 != 0)
						break;
					if (pressed && !status.test_key_pressed) // Solve the need to hold the Test button
						status.test_on = !status.test_on;
					status.test_key_pressed = pressed;
					break;
				case usio_btn::coin:
					// Two coin slots per board: the first player on the board feeds
					// coin1, the second (Tekken P2) feeds coin2. Each slot keeps its
					// own debounce so one press inserts exactly one coin.
					if (player % 2 == 0)
					{
						if (pressed && !status.coin_key_pressed)
							status.coin_counter++;
						status.coin_key_pressed = pressed;
					}
					else
					{
						if (pressed && !status.coin_key_pressed2)
							status.coin_counter2++;
						status.coin_key_pressed2 = pressed;
					}
					break;
				case usio_btn::service:
					if (player % 2 == 0 && pressed)
						input |= 0x4000;
					break;
				case usio_btn::enter:
					if (pressed)
					{
						input |= (m_is_tekken6 ? 0x8000ULL : 0x800000ULL) << shift;
						if (player == 0)
							digital_input_lm |= 0x800;
					}
					break;
				case usio_btn::up:
					if (pressed)
					{
						input |= (m_is_tekken6 ? 0x2000ULL : 0x200000ULL) << shift;
						if (player == 0)
							digital_input_lm |= 0x200;
					}
					break;
				case usio_btn::down:
					if (pressed)
					{
						input |= (m_is_tekken6 ? 0x1000ULL : 0x100000ULL) << shift;
						if (player == 0)
							digital_input_lm |= 0x400;
					}
					break;
				case usio_btn::left:
					if (pressed)
					{
						input |= (m_is_tekken6 ? 0x800ULL : 0x80000ULL) << shift;
						if (player == 0)
							digital_input_lm |= 0x2000;
					}
					break;
				case usio_btn::right:
					if (pressed)
					{
						input |= (m_is_tekken6 ? 0x400ULL : 0x40000ULL) << shift;
						if (player == 0)
							digital_input_lm |= 0x4000;
					}
					break;
				case usio_btn::tekken_button1:
					if (pressed)
					{
						input |= (m_is_tekken6 ? 0x200ULL : 0x20000ULL) << shift;
						if (player == 0)
							digital_input_lm |= 0x100;
					}
					break;
				case usio_btn::tekken_button2:
					if (pressed)
						input |= (m_is_tekken6 ? 0x100ULL : 0x10000ULL) << shift;
					break;
				case usio_btn::tekken_button3:
					if (pressed)
						input |= (m_is_tekken6 ? 0x800000ULL : 0x40000000ULL) << shift;
					break;
				case usio_btn::tekken_button4:
					if (pressed)
						input |= (m_is_tekken6 ? 0x400000ULL : 0x20000000ULL) << shift;
					break;
				case usio_btn::tekken_button5:
					if (pressed)
						input |= (m_is_tekken6 ? 0x200000ULL : 0x80000000ULL) << shift;
					break;
				case usio_btn::card_tapping:
					if (pressed)
						tap_card(player);
					break;
				default:
					break;
				}
			});
		}

		if (player % 2 == 0 && status.test_on)
		{
			input |= 0x80;
			if (player == 0)
				digital_input_lm |= 0x1000;
		}
	};

	for (usz i = 0; i < m_io_status.size(); i++)
		m_io_status[i].card_tapped = false;
	for (usz i = 0; i < g_cfg_usio.players.size(); i++)
		translate_from_pad(i, i);

	for (usz i = 0; i < 2; i++)
	{
		// System 357 reads P1 from board 0 (0x80) / P2 from board 1 (0x100),
		// same as the gun games. The original code wrote them swapped (P1 ->
		// 0x100), so on System 357 titles like Tekken 6 the game read board 0
		// where nothing was written and no input registered.
		std::memcpy(input_buf.data() + 0x80 + i * 0x80, &digital_input[i], sizeof(u64));
		// Two coin slots per board: coin1 at +0x10, coin2 at +0x12 (Tekken P2).
		std::memcpy(input_buf.data() + 0x80 + i * 0x80 + 0x10, &m_io_status[i].coin_counter, sizeof(u16));
		std::memcpy(input_buf.data() + 0x80 + i * 0x80 + 0x12, &m_io_status[i].coin_counter2, sizeof(u16));
	}

	std::memcpy(input_buf.data(), &digital_input_lm, sizeof(u16));

	input_buf[2] = 0b00010000; // DIP switches, 8 in total

	response = std::move(input_buf);
}

void usb_device_usio::emulate_card_reader(std::vector<u8>& buf, u16 reg)
{
	static std::array<std::vector<u8>, 2> pending_response = {};
	usz reader_index = 0;

	const auto calculate_checksum = [](bool check, std::vector<u8>& data) -> bool
	{
		if (data.size() < 0x06)
			return false;

		const usz data_end = data.size() - 2;
		u8 sum = data[3] + data[4];

		for (usz i = 5; i < data_end; i++)
			sum -= data[i];

		if (check)
			return *reinterpret_cast<le_t<u16>*>(&data[data_end]) == sum;

		*reinterpret_cast<le_t<u16>*>(&data[data_end]) = sum;
		return true;
	};

	switch (reg)
	{
	case 0x0080:
	case 0x0090:
	{
		reader_index = reg == 0x0080 ? 0 : 1;
		buf = {0x02, 0x03, 0x00, 0x00, 0xFF, 0x0F, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x10, 0x00};
		*reinterpret_cast<le_t<u16>*>(buf.data() + 2) = ::narrow<u16>(pending_response[reader_index].size());
		break;
	}
	case 0x7000:
	case 0x7800:
	{
		reader_index = reg == 0x7000 ? 0 : 1;
		buf = std::move(pending_response[reader_index]);
		pending_response[reader_index].clear(); // Ensure its empty state after being moved
		break;
	}
	case 0x7400:
	case 0x7C00:
	{
		if (!calculate_checksum(true, buf))
			break;
		reader_index = reg == 0x7400 ? 0 : 1;
		const auto& status = ::at32(m_io_status, reader_index);
		const usz card_player = reader_index * 2 + status.card_index;
		const u8 payload_length = buf[3];
		const u8 command = buf[4];
		const u8* const payload = &buf[6];
		switch (command)
		{
		case 0xE8:
		{
			pending_response[reader_index] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x0D, 0xF3, 0xD5, 0x07, 0xDC, 0xF4, 0x3F, 0x11, 0x4D, 0x85, 0x61, 0xF1, 0x26, 0x6A, 0x87, 0xC9, 0x00};
			break;
		}
		case 0xEE:
		{
			pending_response[reader_index] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x0A, 0xF6, 0xD5, 0x07, 0xFF, 0x3F, 0x0E, 0xF1, 0xFF, 0x3F, 0x0E, 0xF1, 0xAA, 0x00};
			break;
		}
		case 0xF1:
		{
			pending_response[reader_index] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x03, 0xFD, 0xD5, 0x41, 0x00, 0xEA, 0x00};
			break;
		}
		case 0xF2:
		{
			pending_response[reader_index] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x02, 0xFE, 0xD5, 0x33, 0xF8, 0x00};
			break;
		}
		case 0xF7:
		{
			pending_response[reader_index] = {0x00, 0x00, 0xFF, 0x03, 0xFD, 0xD5, 0x4B, 0x00, 0xE0, 0x00};
			break;
		}
		case 0xFA:
		{
			pending_response[reader_index] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x02, 0xFE, 0xD5, 0x33, 0xF8, 0x00};
			break;
		}
		case 0xFB:
		{
			if (payload_length >= 5)
			{
				if (*reinterpret_cast<const le_t<u16>*>(&payload[0]) == 0x0140)
				{
					if (payload[3] < 4)
					{
						pending_response[reader_index] = {0x00, 0x00, 0xFF, 0x13, 0xED, 0xD5, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEA, 0x00};
						std::memcpy(pending_response[reader_index].data() + 8, g_fxo->get<usio_memory>().card_data[card_player].data() + payload[3] * 0x10, 0x10);
					}
					else
					{
						pending_response[reader_index] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x03, 0xFD, 0xD5, 0x41, 0x13, 0xD7, 0x00};
					}
				}
				else
				{
					pending_response[reader_index] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x03, 0xFD, 0xD5, 0x09, 0x00, 0x22, 0x00};
				}
			}
			break;
		}
		case 0xFC:
		{
			if (payload_length >= 2)
			{
				switch (payload[0])
				{
				case 0x52:
					pending_response[reader_index] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x04, 0xFC, 0xD5, 0x53, 0x01, 0x00, 0xD7, 0x00};
					break;
				case 0x0E:
					pending_response[reader_index] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x02, 0xFE, 0xD5, 0x0F, 0x1C, 0x00};
					break;
				case 0x4A:
					if (status.card_tapped)
					{
						pending_response[reader_index] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x0C, 0xF4, 0xD5, 0x4B, 0x01, 0x01, 0x00, 0x04, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0xCE, 0x00};
						std::memcpy(pending_response[reader_index].data() + 0x13, g_fxo->get<usio_memory>().card_data[card_player].data(), 4);
					}
					else
					{
						pending_response[reader_index] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x03, 0xFD, 0xD5, 0x4B, 0x00, 0xE0, 0x00};
					}
					break;
				case 0x32:
					pending_response[reader_index] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x02, 0xFE, 0xD5, 0x33, 0xF8, 0x00};
					break;
				default:
					break;
				}
			}
			break;
		}
		case 0xFD:
		{
			if (payload_length >= 2)
			{
				switch (payload[0])
				{
				case 0x18:
					pending_response[reader_index] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x02, 0xFE, 0xD5, 0x19, 0x12, 0x00};
					break;
				case 0x12:
					pending_response[reader_index] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x02, 0xFE, 0xD5, 0x13, 0x18, 0x00};
					break;
				default:
					break;
				}
			}
			break;
		}
		case 0xFE:
		{
			pending_response[reader_index] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x05, 0xFB, 0xD5, 0x0D, 0x00, 0x06, 0x00, 0x18, 0x00};
			break;
		}
		case 0xFF:
		{
			pending_response[reader_index] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
			break;
		}
		default:
		{
			usio_log.trace("Unhandled card reader command: 0x%02X", command);
			break;
		}
		}
		calculate_checksum(false, pending_response[reader_index]);
		break;
	}
	default:
		break;
	}
}

void usb_device_usio::tap_card(usz player)
{
	auto& status = ::at32(m_io_status, player / 2);
	status.card_tapped = true;
	status.card_index = player % 2;
}

void usb_device_usio::usio_write(u8 channel, u16 reg, std::vector<u8>& data)
{
	const auto get_u16 = [&](std::string_view usio_func) -> u16
	{
		if (data.size() != 2)
		{
			usio_log.error("data.size() is %d, expected 2 for get_u16 in %s", data.size(), usio_func);
		}
		return *reinterpret_cast<const le_t<u16>*>(data.data());
	};

	if (channel == 0)
	{
		switch (reg)
		{
		case 0x0002:
		{
			usio_log.trace("SetSystemError: 0x%04X", get_u16("SetSystemError"));
			break;
		}
		case 0x000A:
		{
			if (get_u16("ClearSram") == 0x6666)
			    usio_log.trace("ClearSram");
			break;
		}
		case 0x0028:
		{
			usio_log.trace("SetExpansionMode: 0x%04X", get_u16("SetExpansionMode"));
			// If we don't set this as requested, razing storm will run at low fps because it will keep trying to change
			// the expansion mode every frame (or perhaps even more often). If this is set, it saves AT LEAST 2 usio transfer calls every time
			expansion_mode = get_u16("SetExpansionMode");
			break;
		}
		case 0x0048:
		case 0x0058:
		case 0x0068:
		case 0x0078:
		{
			usio_log.trace("SetHopperRequest(Hopper: %d, Request: 0x%04X)", (reg - 0x48) / 0x10, get_u16("SetHopperRequest"));
			hoppers[(reg - 0x48) / 0x10] = get_u16("SetHopperLimit");
			break;
		}
		case 0x004A:
		case 0x005A:
		case 0x006A:
		case 0x007A:
		{
			usio_log.trace("SetHopperLimit(Hopper: %d, Limit: 0x%04X)", (reg - 0x4A) / 0x10, get_u16("SetHopperLimit"));
			break;
		}
		case 0x1400:
		{
			// for razing storm, LED outputs in the test menu seem to change the bytes at [0x10] and [0x11]
			usio_log.trace("SetLed");
			break;
		}
		case 0x0080:
		case 0x008D:
		case 0x0090:
		case 0x009D:
		case 0x7400:
		case 0x7C00:
		{
			emulate_card_reader(data, reg);
			break;
		}
		default:
		{
			usio_log.trace("Unhandled channel 0 register write(reg: 0x%04X, size: 0x%04X, data: %s)", reg, data.size(), fmt::buf_to_hexstring(data.data(), data.size()));
			break;
		}
		}
	}
	else if (channel >= 2)
	{
		const u8 page = channel - 2;
		usio_log.trace("Usio write of sram(page: 0x%02X, addr: 0x%04X, size: 0x%04X, data: %s)", page, reg, data.size(), fmt::buf_to_hexstring(data.data(), data.size()));
		auto& memory = g_fxo->get<usio_memory>().backup_memory;
		const usz addr_end = reg + data.size();
		if (data.size() > 0 && page < usio_memory::page_count && addr_end <= usio_memory::page_size)
			std::memcpy(&memory[usio_memory::page_size * page + reg], data.data(), data.size());
		else
			usio_log.error("Usio sram invalid write operation(page: 0x%02X, addr: 0x%04X, size: 0x%04X, data: %s)", page, reg, data.size(), fmt::buf_to_hexstring(data.data(), data.size()));
	}
	else
	{
		// Channel 1 is the endpoint for firmware update.
		// We are not using any firmware since this is emulation.
		usio_log.trace("Unsupported write operation(channel: 0x%02X, addr: 0x%04X, size: 0x%04X, data: %s)", channel, reg, data.size(), fmt::buf_to_hexstring(data.data(), data.size()));
	}
}

void usb_device_usio::usio_read(u8 channel, u16 reg, u16 size)
{
	if (channel == 0)
	{
		switch (reg)
		{
		case 0x0000:
		{
			// Get Buffer, rarely gives a reply on real HW
			// First U16 seems to be a timestamp of sort
			// Purpose seems related to connectivity check
			// [2] and [3] seem to be a U16 to set specific error flags.
			response = {0x7E, 0xE4, 0x00, 0x00, 0x74, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7E, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
			response.resize(0x100); // Razing storm wants a full 0x100 bytes here, and we need the space to write in the gun data and whatnot

			// IO board count
			response[0x22] = 0x01;

			// The following are mostly used by razing storm, but sending them to other games doesn't seem to hurt anything
			// in the future it might be nice if there was a good way from in here to distinguish which game is running
			// and what kind of io setup the game expects.
			
			// Write the expansion mode obtained from SetExpansionMode here
			response[0x28] = expansion_mode & 0xFF;
			response[0x29] = expansion_mode >> 8;
			
			// For razing storm, this seems to set the current "mode" of something, possibly the gundrive board?
			// 0x9000 is mode 5, which will allow the gun sensor check to work and display NG or OK for each sensor
			// 0x8000 is mode 4, which lets the guns actually work ingame
			// It looks like razing storm sets the mode via the hopper request? (0x48)
			// Although, I don't even know where the hopper label came from in this codebase.
			// It sends 0x8001 to set mode 4, and 0x9001 to set mode 5, but the game does not want the 1 back.
			response[0x40] = 0x00;
			response[0x41] = hoppers[0] >> 8;

			// Razing storm seems to get the "state" of all 10 LED sensors from here, twice because 2 players
			// simply setting them in here does not work though, there's other flags it checks.
			// These are simple bitmasks, and just determine if a sensor is OK or NG
			// I assume it just means if the gun can see the light from the sensor or not?
			response[0x44] = 0xFF; // sensors player 1
			response[0x45] = 0xFF; // sensors player 1
			response[0x46] = 0xFF; // sensors player 2
			response[0x47] = 0xFF; // sensors player 2

			response[0x54] = 0x00; // P1 gun status, needs to be 0 for the gun to work
			response[0x5C] = 0x00; // Same as above but for P2

			u8 analog_data[7] = {0}; // P1X, P1Y, P2X, P2Y

#ifdef HAVE_LIBEVDEV
			// Read absolute aiming position from connected evdev lightguns.
			// Device 0 -> P1, device 1 -> P2. Axes are normalized to 0..255
			// here and scaled up to the 16-bit range the game expects below.
			if (m_is_gun_game)
			{
				usio_gun_handler& gun = g_fxo->get<usio_gun_handler>();
				std::scoped_lock lock(gun.handler.mutex);

				const auto read_axis = [&](u32 gunno, bool is_y) -> u8
				{
					const int max = is_y ? gun.handler.get_axis_y_max(gunno) : gun.handler.get_axis_x_max(gunno);
					if (max <= 0)
						return 0;
					const int val = is_y ? gun.handler.get_axis_y(gunno) : gun.handler.get_axis_x(gunno);
					return static_cast<u8>(std::clamp(val * 255 / max, 0, 255));
				};

				const u32 num_guns = gun.handler.get_num_guns();
				if (num_guns > 0)
				{
					gun.handler.poll(0);
					analog_data[0] = read_axis(0, false);
					analog_data[1] = read_axis(0, true);
				}
				if (num_guns > 1)
				{
					gun.handler.poll(1);
					analog_data[2] = read_axis(1, false);
					analog_data[3] = read_axis(1, true);
				}
			}
#endif

			// razing storm guns:
			u16 p1_x = (analog_data[0] * 65535) / 255;
			u16 p1_y = (analog_data[1] * 65535) / 255;
			u16 p2_x = (analog_data[2] * 65535) / 255;
			u16 p2_y = (analog_data[3] * 65535) / 255;

			response[0x50] = p1_x & 0xFF;
			response[0x51] = p1_x >> 8;
			response[0x52] = p1_y & 0xFF;
			response[0x53] = p1_y >> 8;

			response[0x58] = p2_x & 0xFF;
			response[0x59] = p2_x >> 8;
			response[0x5A] = p2_y & 0xFF;
			response[0x5B] = p2_y >> 8;
			
			break;
		}
		case 0x0080:
		case 0x0090:
		case 0x7000:
		case 0x7800:
		{
			emulate_card_reader(response, reg);
			break;
		}
		case 0x1000:
		{
			// Often called, gets digital input. Tekken and the System 357 gun
			// games share this register but use different JVS bit/board
			// layouts, so pick the right translator based on the title.
			if (m_is_gun_game)
				translate_input_gun();
			else
				translate_input_tekken();
			break;
		}
		case 0x1080:
		{
			// Often called, gets input from usio for Taiko
			translate_input_taiko();
			break;
		}
		case 0x1800:
		case 0x1880:
		{
			// Firmware
			// "NBGI.;USIO01;Ver1.00;JPN,Multipurpose with PPG."
			// This contains information about the Master USIO + connected IO boards.
			// Identifier, command versions, how many bytes per player for switches, how many analog channels and so on.
			// It seems to be losely based on the original JVS spec.
			constexpr std::array<u8, 0x180> info {0x4E, 0x42, 0x47, 0x49, 0x2E, 0x3B, 0x55, 0x53, 0x49, 0x4F, 0x30, 0x31, 0x3B, 0x56, 0x65, 0x72, 0x31, 0x2E, 0x30, 0x30, 0x3B, 0x4A, 0x50, 0x4E, 0x2C, 0x4D, 0x75, 0x6C, 0x74, 0x69, 0x70, 0x75, 0x72, 0x70, 0x6F, 0x73, 0x65, 0x20, 0x77, 0x69, 0x74, 0x68, 0x20, 0x50, 0x50, 0x47, 0x2E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4E, 0x42, 0x47, 0x49, 0x31, 0x3B, 0x55, 0x53, 0x49, 0x4F, 0x30, 0x31, 0x3B, 0x56, 0x65, 0x72, 0x31, 0x2E, 0x30, 0x30, 0x3B, 0x4A, 0x50, 0x4E, 0x2C, 0x4D, 0x75, 0x6C, 0x74, 0x69, 0x70, 0x75, 0x72, 0x70, 0x6F, 0x73, 0x65, 0x20, 0x77, 0x69, 0x74, 0x68, 0x20, 0x50, 0x50, 0x47, 0x2E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x13, 0x00, 0x30, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0x02, 0x00, 0x08, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x03, 0x00, 0x75, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4E, 0x42, 0x47, 0x49, 0x32, 0x3B, 0x55, 0x53, 0x49, 0x4F, 0x30, 0x31, 0x3B, 0x56, 0x65, 0x72, 0x31, 0x2E, 0x30, 0x30, 0x3B, 0x4A, 0x50, 0x4E, 0x2C, 0x4D, 0x75, 0x6C, 0x74, 0x69, 0x70, 0x75, 0x72, 0x70, 0x6F, 0x73, 0x65, 0x20, 0x77, 0x69, 0x74, 0x68, 0x20, 0x50, 0x50, 0x47, 0x2E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x13, 0x00, 0x30, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0x02, 0x00, 0x08, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x03, 0x00, 0x75, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
			response = {info.begin() + (reg - 0x1800), info.end()};
			break;
		}
		default:
		{
			usio_log.trace("Unhandled channel 0 register read(reg: 0x%04X, size: 0x%04X)", reg, size);
			break;
		}
		}
	}
	else if (channel >= 2)
	{
		const u8 page = channel - 2;
		usio_log.trace("Usio read of sram(page: 0x%02X, addr: 0x%04X, size: 0x%04X)", page, reg, size);
		auto& memory = g_fxo->get<usio_memory>().backup_memory;
		const usz addr_end = reg + size;
		if (size > 0 && page < usio_memory::page_count && addr_end <= usio_memory::page_size)
			response.insert(response.end(), memory.begin() + (usio_memory::page_size * page + reg), memory.begin() + (usio_memory::page_size * page + addr_end));
		else
			usio_log.error("Usio sram invalid read operation(page: 0x%02X, addr: 0x%04X, size: 0x%04X)", page, reg, size);
	}
	else
	{
		// Channel 1 is the endpoint for firmware update.
		// We are not using any firmware since this is emulation.
		usio_log.trace("Unsupported read operation(channel: 0x%02X, addr: 0x%04X, size: 0x%04X)", channel, reg, size);
	}

	response.resize(size); // Always resize the response vector to the given size
}

void usb_device_usio::usio_init(u8 channel, u16 reg, u16 size)
{
	if (channel == 0)
	{
		switch (reg)
		{
		case 0x0008:
		{
			usio_log.trace("USIO Reset");
			break;
		}
		case 0x000A:
		{
			usio_log.trace("USIO ClearSram");
			g_fxo->get<usio_memory>().init();
			break;
		}
		default:
		{
			usio_log.trace("Unhandled channel 0 register init(reg: 0x%04X, size: 0x%04X)", reg, size);
			break;
		}
		}
	}
	else
	{
		usio_log.trace("Unsupported init operation(channel: 0x%02X, addr: 0x%04X, size: 0x%04X)", channel, reg, size);
	}
}

void usb_device_usio::interrupt_transfer(u32 buf_size, u8* buf, u32 endpoint, UsbTransfer* transfer)
{
	constexpr u8 USIO_COMMAND_WRITE = 0x90;
	constexpr u8 USIO_COMMAND_READ  = 0x10;
	constexpr u8 USIO_COMMAND_INIT  = 0xA0;

	static bool expecting_data = false;
	static std::vector<u8> usio_data;
	static u32 response_seek = 0;
	static u8 usio_channel   = 0;
	static u16 usio_register = 0;
	static u16 usio_length   = 0;

	transfer->fake            = true;
	transfer->expected_result = HC_CC_NOERR;
	// The latency varies per operation but it doesn't seem to matter for this device so let's go fast!
	transfer->expected_time = get_timestamp() + 1'000;

	is_used = true;

	switch (endpoint)
	{
	case 0x01:
	{
		// Write endpoint
		transfer->expected_count = buf_size;

		if (expecting_data)
		{
			usio_data.insert(usio_data.end(), buf, buf + buf_size);
			usio_length -= buf_size;

			if (usio_length == 0)
			{
				expecting_data = false;
				usio_write(usio_channel, usio_register, usio_data);
			}
			return;
		}

		// Commands
		if (buf_size != 6)
		{
			usio_log.error("Expected a command but buf_size != 6");
			return;
		}

		usio_channel  = buf[0] & 0xF;
		usio_register = *reinterpret_cast<le_t<u16>*>(&buf[2]);
		usio_length   = *reinterpret_cast<le_t<u16>*>(&buf[4]);

		if ((buf[0] & USIO_COMMAND_WRITE) == USIO_COMMAND_WRITE)
		{
			usio_log.trace("UsioWrite(Channel: 0x%02X, Register: 0x%04X, Length: 0x%04X)", usio_channel, usio_register, usio_length);
			if (((~(usio_register >> 8)) & 0xF0) != buf[1])
			{
				usio_log.error("Invalid UsioWrite command");
				return;
			}
			expecting_data = true;
			usio_data.clear();
		}
		else if ((buf[0] & USIO_COMMAND_READ) == USIO_COMMAND_READ)
		{
			usio_log.trace("UsioRead(Channel: 0x%02X, Register: 0x%04X, Length: 0x%04X)", usio_channel, usio_register, usio_length);
			response_seek = 0;
			response.clear();
			usio_read(usio_channel, usio_register, usio_length);
		}
		else if ((buf[0] & USIO_COMMAND_INIT) == USIO_COMMAND_INIT)
		{
			usio_log.trace("UsioInit(Channel: 0x%02X, Register: 0x%04X, Length: 0x%04X)", usio_channel, usio_register, usio_length);
			usio_init(usio_channel, usio_register, usio_length);
		}
		else
		{
			usio_log.error("Received an unexpected command: 0x%02X", buf[0]);
		}
		break;
	}
	case 0x82:
	{
		// Read endpoint
		const u32 size = std::min(buf_size, static_cast<u32>(response.size() - response_seek));
		memcpy(buf, response.data() + response_seek, size);
		response_seek += size;
		transfer->expected_count = size;
		break;
	}
	default:
		usio_log.error("Unhandled endpoint: 0x%x", endpoint);
		break;
	}
}
