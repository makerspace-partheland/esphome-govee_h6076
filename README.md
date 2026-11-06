# Govee H6076 for ESPHome

[Deutsch](#deutsch) | [English](#english)

## Deutsch

Die H6076 wird als „Govee RGBICW Smart Stehlampe Basic“ verkauft.

Die ESPHome-Komponente `govee_h6076` steuert die Govee-H6076-Stehlampe lokal. Home Assistant bindet sie über die ESPHome-API ein. Sie unterstützt RGB, Warmweiß, Helligkeit, sieben Farbsegmente, Szenen und Musikmodi. Schlafmodus und Bluetooth-Proxy sind ebenfalls enthalten.

Die optionale [DreamView-Steuerung](examples/H6076.md#dreamview) verbindet H6076-Lampen zu einer Gruppe für gemeinsame Lichteffekte zur Musik. Eine Lampe übernimmt die Steuerung. Nur in der YAML-Konfiguration dieser Steuerlampe werden die Bluetooth-MAC-Adressen der Teilnehmer eingetragen. Die anderen Lampen benötigen keine Teilnehmerliste. Die Gruppe wird aus dieser Liste automatisch eingerichtet. In Home Assistant wird die Synchronisierung ein- und ausgeschaltet.

ESPHome wird auf den **ESP8685** geflasht. Der **FR8018HD** steuert weiterhin LEDs, Taster und Mikrofon. Beide Chips kommunizieren über UART.

Die unterstützte Platine ist `H6076迭代_MAIN_V1.0` mit Aufdruck `2024/12/02` und 4 MB Flash. Für die Konfiguration wird ESP-IDF verwendet.

![H6076-Platine mit Chips und Anschlüssen](docs/assets/platinenuebersicht.svg)

- [Flashen: Chips, Pins und Anschluss](docs/installation.md#deutsch)
- [Konfiguration einrichten](examples/H6076.md#deutsch)
- [Vollständige Geräte-YAML mit deutschen Anzeigenamen](examples/h6076.de.yaml)

---

## English

The H6076 is sold as [Govee RGBICW Smart Floor Lamp Basic](https://uk.govee.com/products/govee-rgbicw-smart-corner-floor-lamp).

The `govee_h6076` component provides local control of the Govee H6076 floor lamp through ESPHome and Home Assistant. It supports RGB, warm white, brightness, seven color segments, scenes and music modes. Sleep mode and a Bluetooth proxy are also included.

The optional [DreamView controls](examples/H6076.md#dreamview-1) connect H6076 lamps in a group for shared lighting effects that respond to music. One lamp acts as the controller. The members' Bluetooth MAC addresses are listed only in the controller lamp's YAML configuration. The other lamps do not need a member list. The group is set up automatically from this list. Home Assistant turns synchronization on and off.

Flash ESPHome onto the **ESP8685**. The **FR8018HD** continues to control the LEDs, button and microphone. The chips communicate over UART.

The supported board is `H6076迭代_MAIN_V1.0`, labeled `2024/12/02`, with 4 MB flash. The configuration uses ESP-IDF.

![H6076 board with chips and connections](docs/assets/platinenuebersicht.svg)

- [Flashing: chips, pins and connections](docs/installation.md#english)
- [Set up the configuration](examples/H6076.md#english)
- [Complete device YAML with English display names](examples/h6076.en.yaml)

---

## Quellen und Lizenzen / Sources and licenses

- [Quellen / Sources](UPSTREAM.md)
- [scroom/ha-govee-local-segments: Apache-2.0](THIRD-PARTY-LICENSES.md#apache-20)
- [Szenenkatalog: MIT / Scene catalogue: MIT](THIRD-PARTY-LICENSES.md#mit)
