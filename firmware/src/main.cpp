/* Read the Adafruit SCD-40 (CO2 / temperature / humidity) over I2C via Zephyr's
 * sensor API — on demand from the shell, not on a timer.
 *
 * Sampling is driven by Zephyr's built-in sensor shell (CONFIG_SENSOR_SHELL):
 *
 *     sensor get scd40@62          # all channels
 *     sensor get scd40@62 co2      # a single channel
 *
 * The in-tree scd4x driver auto-starts periodic measurement at boot; a fresh
 * sample lands roughly every 5 s. sensor_sample_fetch() returns 0 WITHOUT
 * updating the values when no new sample is ready, so a `sensor get` issued in
 * the first ~5 s after boot may report stale/zero data.
 *
 * main() only proves the device is present, then returns — the kernel keeps
 * running and the shell thread serves commands.
 *
 * C++ app (see docs/language-cpp.md): the sensor API is a C API called directly
 * from C++. `main` is never name-mangled, so it needs no extern "C".
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
/* C header, not <cstdio>: the minimal C++ runtime (no CONFIG_REQUIRES_FULL_LIBCPP)
 * ships no <c*> wrapper headers — see docs/language-cpp.md. */
#include <stdio.h>

int main(void)
{
	const struct device *const scd40 = DEVICE_DT_GET(DT_NODELABEL(scd40));

	if (!device_is_ready(scd40)) {
		printf("SCD-40 not ready\n");
		return 0;
	}

	printf("SCD-40 online — read it with `sensor get scd40@62` "
	       "(first valid sample ~5 s after boot)\n");
	return 0;
}
