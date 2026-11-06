[Deutsch](#deutsch) | [English](#english)

# Deutsch

## Herkunft und Lizenzen

Die ESPHome-Komponente wurde eigenständig in C++ und Python entwickelt. Eigene Analysen der UART-Kommunikation zwischen ESP8685 und FR8018HD sowie Gerätetests ergänzten die verwendeten Upstream-Protokollangaben.

- Protokollreferenz: [scroom/ha-govee-local-segments](https://github.com/scroom/ha-govee-local-segments), Stand `997a72e85e6613b270aa66b9f0694cdfb039e725`, Datei `custom_components/govee_light_local/music.py`. Ausgangspunkt für Musikbefehle und Paketformate. Lizenz des Referenzprojekts: [Apache-2.0](THIRD-PARTY-LICENSES.md#apache-20).

- Übernommene Daten: [teh-hippo/ha-govee-led-ble](https://github.com/teh-hippo/ha-govee-led-ble), Stand `cecb397844d5b97a25dae976a482ac0aa83a2c0c`, Datei `custom_components/ha_govee_led_ble/scene_catalogues/H6076.json`. Namen, Szenencodes und Animationsdaten wurden in `scene_catalogue.py` übernommen. `transport.py`, `native_scenes.py` und `tools/ble/kaitai/command_write.ksy` dienten als Referenzen für das A3-Paketformat. Lizenz des Szenenkatalogs: [MIT, Copyright 2024 teh-hippo](THIRD-PARTY-LICENSES.md#mit).

---

# English

## Provenance and licenses

The ESPHome component was independently developed in C++ and Python. Analyses of UART communication between the ESP8685 and FR8018HD, together with device tests, supplemented the upstream protocol information.

- Protocol reference: [scroom/ha-govee-local-segments](https://github.com/scroom/ha-govee-local-segments), revision `997a72e85e6613b270aa66b9f0694cdfb039e725`, file `custom_components/govee_light_local/music.py`. Starting point for music commands and packet formats. Reference project license: [Apache-2.0](THIRD-PARTY-LICENSES.md#apache-20).

- Copied data: [teh-hippo/ha-govee-led-ble](https://github.com/teh-hippo/ha-govee-led-ble), revision `cecb397844d5b97a25dae976a482ac0aa83a2c0c`, file `custom_components/ha_govee_led_ble/scene_catalogues/H6076.json`. Names, scene codes and animation data were copied into `scene_catalogue.py`. `transport.py`, `native_scenes.py` and `tools/ble/kaitai/command_write.ksy` served as references for the A3 packet format. Scene catalogue license: [MIT, Copyright 2024 teh-hippo](THIRD-PARTY-LICENSES.md#mit).
