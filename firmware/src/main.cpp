/* Read the Adafruit SCD-40 (CO2 / temperature / humidity) over I2C via Zephyr's
 * sensor API and print each reading to the console (~every 5 s).
 *
 * The in-tree scd4x driver auto-starts periodic measurement at boot; a fresh
 * sample lands roughly every 5 s. sensor_sample_fetch() returns 0 WITHOUT
 * updating the values when no new sample is ready, so the first reads after boot
 * may show stale/zero data — we poll on a 5 s cadence to match the sensor.
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

namespace {
constexpr k_timeout_t kSamplePeriod = K_SECONDS(5);
}  // namespace

int main(void)
{
	const struct device *const scd40 = DEVICE_DT_GET(DT_NODELABEL(scd40));

	if (!device_is_ready(scd40)) {
		printf("SCD-40 not ready\n");
		return 0;
	}
	printf("SCD-40 online — first valid sample in ~5 s\n");

	while (true) {
		struct sensor_value co2, temp, hum;
		int rc = sensor_sample_fetch(scd40);

		if (rc != 0) {
			printf("sample_fetch failed: %d\n", rc);
			k_sleep(kSamplePeriod);
			continue;
		}

		sensor_channel_get(scd40, SENSOR_CHAN_CO2, &co2);
		sensor_channel_get(scd40, SENSOR_CHAN_AMBIENT_TEMP, &temp);
		sensor_channel_get(scd40, SENSOR_CHAN_HUMIDITY, &hum);

		printf("CO2: %.0f ppm  |  T: %.2f C  |  RH: %.1f %%\n",
		       sensor_value_to_double(&co2),
		       sensor_value_to_double(&temp),
		       sensor_value_to_double(&hum));

		k_sleep(kSamplePeriod);
	}
	return 0;
}
