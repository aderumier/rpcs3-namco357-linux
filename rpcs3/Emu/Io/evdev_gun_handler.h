#pragma once
#ifdef HAVE_LIBEVDEV

#include <map>
#include <string>
#include <vector>
#include "Utilities/mutex.h"

enum class gun_button
{
	btn_left,
	btn_right,
	btn_middle,
	btn_1,
	btn_2,
	btn_3,
	btn_4,
	btn_5,
	btn_6,
	btn_7,
	btn_8
};

class evdev_gun_handler
{
public:
	evdev_gun_handler();
	~evdev_gun_handler();

	bool init();

	bool is_init() const;
	u32 get_num_guns() const;
	int get_button(u32 gunno, gun_button button) const;
	int get_axis_x(u32 gunno) const;
	int get_axis_y(u32 gunno) const;
	int get_axis_x_max(u32 gunno) const;
	int get_axis_y_max(u32 gunno) const;
	const std::string& get_devnode(u32 gunno) const;

	void poll(u32 index);

	shared_mutex mutex;

private:
	atomic_t<bool> m_is_init{false};
	struct udev* m_udev = nullptr;

	struct evdev_axis
	{
		int value = 0;
		int min = 0;
		int max = 0;
	};

	struct evdev_gun
	{
		struct libevdev* device = nullptr;
		std::string devnode; // e.g. /dev/input/event29
		std::map<int, int> buttons;
		std::map<int, evdev_axis> axis;

		// Relative mode (touchpads): the device reports absolute finger position,
		// but we integrate the finger-drag deltas into axis[].value (a virtual
		// pointer) instead of using the raw position, so it works like a
		// trackball. last_raw/synced track the previous finger sample so a lift +
		// re-touch doesn't jump.
		bool relative = false;
		bool rel_axes = false; // true: REL_X/REL_Y mouse (no absolute position; axes are synthetic)
		int last_raw_x = 0;
		int last_raw_y = 0;
		bool synced_x = false;
		bool synced_y = false;
	};

	std::vector<evdev_gun> m_devices;
};

#endif
