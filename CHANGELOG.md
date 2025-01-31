# Changelog

## 3.4.1 (June 23rd, 2023)

### Features
- (commercial feature only) Added support for bootstrapping from SIM card on
  nRF9160-based devices
- Added support for nRF700x Wi-FI IC
- Added Light Control object for LED handling
- Added persistence of attribute storage
- Added support for FOTA of application and modem firmware for nRF9160 using
  experimental Advanced Firmware Update object (/33629)
- (commercial feature only) Added support for Core Peristence

### Improvements
- Updated Anjay to version 3.4.1
- Fixed avs_commons and anjay_zephyr configurations dependencies
- Updated Nordic Location Services-related APIs and object implementations to match new object definitions and server-side behavior
- Kconfig options which associated values can be changed during runtime have been marked as defaults
- Fixed compatibility with NCS v2.3.0

## 3.3.0 (Feb 21st, 2023)

### Improvements
- Updated Anjay to version 3.3.0
- Revamped config of Anjay and its dependencies
- Moved common code from demo samples to Anjay Zephyr
- Added runtime certificate configuration option

## 3.2.1 (Dec 13th, 2022)

### Bugfixes
- Deleted second ANJAY_WITH_MODULE_FACTORY_PROVISIONING definition in anjay_config.h
- Added correct usage of the `ZSOCK_MSG_DONTWAIT` flag in network integration

### Improvements
- Added ANJAY_WITH_COMMUNICATION_TIMESTAMP_API, ANJAY_WITH_OBSERVATION_STATUS
  and ANJAY_MAX_OBSERVATION_SERVERS_REPORTED_NUMBER to Kconfig
- Fixed compatibility with newer versions of Zephyr that require including its
  headers from the `zephyr/` directory
- Added support for private keys stored as DER-encoded PKCS#1 and SECG1
  (in addition to previously supported PKCS#8 and PEM encodings) on nRF91
  security backend

## 3.1.2 (Aug 31st, 2022)

### Bugfixes
- Fixed dangerous usage of `avs_realloc()` in hardware TLS socket integration
- Anjay log level can be properly configured through Kconfig

### Improvements
- Added support for platforms that don't support `CONFIG_DATE_TIME`
- Simplified underlying socket creation in `compat/net_impl.c`
- Updated Anjay to version 3.1.2

## 3.1.1 (Aug 5th, 2022)

### Improvements
- Updated Anjay to version 3.1.1
- Updated to Zephyr 3.x

## 3.0.0 (Jun 21st, 2022)

### Features
- Added modem PSK credentials store/remove options
- Added configurability of ANJAY_WITH_MODULE_FACTORY_PROVISIONING

### Improvements
- Updated Anjay to version 3.0.0
- Use nRF Modem library to retrieve modem firmware version

## 2.14.1 (Mar 24th, 2022)

### Features
- Added support for Zephyr (and nRF Connect SDK) offloaded (D)TLS sockets

### Improvements
- Updated Anjay to version 2.14.1
- Migrated logging to use the native Zephyr logger
- Moved Mbed TLS entropy handling to this module
- Updated support for newer versions of Mbed TLS, Zephyr and nRF Connect SDK

## 2.14.0 (Oct 8th, 2021)

### Improvements
- Updated Anjay to version 2.14.0
- Added native Zephyr threading integration

## 2.13.0 (Jul 30th, 2021)

### Improvements
- Updated Anjay to version 2.13.0
- Improved socket handling layer
- Moved Anjay configuration to KConfig
- Compatible with Zephyr 2.6.0

## 2.11.1 (Jun 9th, 2021)

### Features
- Initial release using Anjay 2.11.1
