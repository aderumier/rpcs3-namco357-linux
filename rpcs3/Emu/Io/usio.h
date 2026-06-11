#pragma once

#include "Emu/system_utils.hpp"
#include "Emu/Io/usb_device.h"

class usb_device_usio : public usb_device_emulated
{
public:
	usb_device_usio(const std::array<u8, 7>& location);
	~usb_device_usio();

	static std::shared_ptr<usb_device> make_instance(u32 controller_index, const std::array<u8, 7>& location);
	static u16 get_num_emu_devices();

	void control_transfer(u8 bmRequestType, u8 bRequest, u16 wValue, u16 wIndex, u16 wLength, u32 buf_size, u8* buf, UsbTransfer* transfer) override;
	void interrupt_transfer(u32 buf_size, u8* buf, u32 endpoint, UsbTransfer* transfer) override;

private:
	void load_backup();
	void save_backup();
	void translate_input_taiko();
	void translate_input_tekken();
	void translate_input_gun();
	void emulate_card_reader(std::vector<u8>& buf, u16 reg);
	void tap_card(usz player);
	void usio_write(u8 channel, u16 reg, std::vector<u8>& data);
	void usio_read(u8 channel, u16 reg, u16 size);
	void usio_init(u8 channel, u16 reg, u16 size);

private:
	bool is_used = false;
	const std::string usio_backup_path = rpcs3::utils::get_hdd1_dir() + "/caches/usiobackup.bin";
	std::vector<u8> response;
	u16 expansion_mode = 0x8000;
	std::array<u16, 4> hoppers = {0x0080, 0, 0, 0};

	struct io_status
	{
		bool test_on = false;
		bool test_key_pressed = false;
		bool coin_key_pressed = false;
		bool coin_key_pressed2 = false; // second coin slot on the same board (Tekken P2 / coin2)
		bool card_tapped = false;
		le_t<u16> coin_counter = 0;
		le_t<u16> coin_counter2 = 0;
		usz card_index = 0;
	};

	std::array<io_status, 2> m_io_status;

	// Separate debounce for the lightgun coin button so it doesn't clobber the
	// pad coin handler's state (they share m_io_status[].coin_counter).
	bool m_gun_coin_pressed = false;

	// Cached at construction: true for Namco System 357 lightgun games, which
	// use the raw JVS layout (translate_input_gun) instead of the Tekken one.
	bool m_is_gun_game = false;
	// Razing Storm uses a different JVS bit layout than the other gun games
	// (Dead Storm / Sailor Zombie / Dark Escape, which share one layout).
	bool m_is_razingstorm = false;
	// Tekken 6 / BR use a different JVS switch layout than Tekken Tag 2.
	bool m_is_tekken6 = false;

	// DeadStorm Pirates steering wheel, emulated as an incremental rotary
	// encoder: holding Left/Right keeps changing the value (the game reads the
	// signed delta between polls), springing back to centre (128) on release.
	double m_wheel_pos = 128.0;
	u64 m_wheel_time_ns = 0;
};
