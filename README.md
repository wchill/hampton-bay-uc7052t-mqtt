# Hampton Bay Fan MQTT

## Overview
ESP8266 project enabling MQTT control for a Hampton Bay fan with a UC7052T wireless receiver. Wireless communication is performed with a CC1101 wireless transceiver operating at 303 MHz.

This will also monitor for Hampton Bay RF signals so the state will stay in sync even if the original remote is used to control the fan.

Fan control is not limited to a single dip switch setting, so up to 16 fans can be controlled with one ESP8266.

## Dependencies
This project uses the following libraries that are available through the Arduino IDE
* [SmartRC-CC1101-Driver-Lib](https://github.com/LSatan/SmartRC-CC1101-Driver-Lib) by LSatan
* [rc-switch](https://github.com/sui77/rc-switch) by sui77
* [PubSubClient](https://pubsubclient.knolleary.net/) by Nick O'Leary

Original source code is from the following project
* [hampton-bay-fan-mqtt](https://github.com/owenb321/hampton-bay-fan-mqtt) by owenb321

## Hardware
* ESP8266 development board (Tested with a NodeMCU v2 and a D1 Mini)
* CC1101 wireless transceiver
  * Wiring info can be found in the [SmartRC-CC1101-Driver-Lib readme](https://github.com/LSatan/SmartRC-CC1101-Driver-Lib#wiring)

## Setup
### Configuration
Make a copy of settings.h.example, make the needed changes, and save as settings.h.
### MQTT
By default, the state/command topics will be
* Fan on/off (payload `ON` or `OFF`)
  * `home/hamptonbay/<fan_id>/fan/state`
  * `home/hamptonbay/<fan_id>/fan/set`
* Fan speed (payload `low`, `medium`, or `high`)
  * `home/hamptonbay/<fan_id>/speed/state`
  * `home/hamptonbay/<fan_id>/speed/set`
* Fan direction (payload `forward` or `reverse`)
  * `home/hamptonbay/<fan_id>/direction/state`
  * `home/hamptonbay/<fan_id>/direction/set`
* Light on/off (payload `ON` or `OFF`)
  * `home/hamptonbay/<fan_id>/light/state`
  * `home/hamptonbay/<fan_id>/light/set`

`fan_id` is a 4-digit binary number determined by the dip switch settings on the transmitter/receiver where up = 1 and down = 0. For example, the dip setting:

|1|2|3|4|
|-|-|-|-|
|↑|↓|↓|↓|

...corresponds to a fan ID of `1000`

Note that because of how the remote commands work, toggling the light or fan direction is not an idempotent operation. If needed, you can write to the state topic to synchronize the state.

### Home Assistant
If `ENABLE_AUTODISCOVERY` is defined (the default), config topics will be published to MQTT under the topics `homeassistant/fan/hamptonbay_<id>_fan/config` and `homeassistant/light/hamptonbay_<id>_light/config` so Home Assistant can auto discover the ceiling fan as a MQTT Fan and MQTT Light. The publish happens only after at least one message is written to a fan's state topic, either manually or via receiving a transmission from a physical remote.

To manually use this in Home Assistant as an MQTT Fan and MQTT Light, an example follows:
```yaml
fan:
- platform: mqtt
  name: "Bedroom Fan"
  state_topic: "home/hamptonbay/1000/fan/state"
  command_topic: "home/hamptonbay/1000/fan/set"
  preset_mode_state_topic: "home/hamptonbay/1000/speed/state"
  preset_mode_command_topic: "home/hamptonbay/1000/speed/set"
  preset_modes:
    - low
    - medium
    - high

light:
- platform: mqtt
  name: "Bedroom Fan Light"
  state_topic: "home/hamptonbay/1000/light/state"
  command_topic: "home/hamptonbay/1000/light/set"
```

Note that setting the fan direction via Home Assistant's MQTT integration is not available out of the box.

### Other
If `ENABLE_OTA` is set, the device can be updated via ArduinoOTA. `HOSTNAME`, `OTA_PASSWORD`, and `OTA_PORT` should also be set appropriately.

If `ENABLE_SPIWRITEREG` is set, writing to the topic `home/hamptonbay/1337/spiwrite/<address>` will write a byte to that SPI address. The address and payload should be both decimal integers (not hex or binary). This is useful to experiment with configuring the CC1101 transceiver without having to reflash the firmware.

If `ENABLE_DEBUGTOPIC` is set, whenever the device detects a transmission from a remote, it will write the details of that transmission to `home/hamptonbay/last_message`.