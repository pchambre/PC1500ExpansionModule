// PC-1500 BLE app -- step 0 spike (2026-09-27): proves the laptop can be a
// BLE peripheral hosting the "PC-1500 Link" GATT service (see
// RP2350/BLE_PROTOCOL.md, to come). It advertises the service, logs every
// write to RX, and echoes it back as a TX notification, so a central such
// as nRF Connect (or, later, a PC-1500) can check both directions.
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:ble_peripheral/ble_peripheral.dart';
import 'package:flutter/material.dart';

/// The PC-1500 Link service and its two characteristics. Fixed for good:
/// firmware, emulator and app all use these.
const linkServiceUuid = 'c31f0001-92a3-40ab-b63d-7cdb0a37aed0';
const linkRxUuid = 'c31f0002-92a3-40ab-b63d-7cdb0a37aed0'; // central -> peripheral, write w/o response
const linkTxUuid = 'c31f0003-92a3-40ab-b63d-7cdb0a37aed0'; // peripheral -> central, notify

const advertisedName = 'PC1500-SRV';

void main() => runApp(const SpikeApp());

class SpikeApp extends StatelessWidget {
  const SpikeApp({super.key});

  @override
  Widget build(BuildContext context) => MaterialApp(
        title: 'PC-1500 BLE',
        theme: ThemeData(colorSchemeSeed: Colors.indigo, useMaterial3: true),
        home: const SpikePage(),
      );
}

class SpikePage extends StatefulWidget {
  const SpikePage({super.key});

  @override
  State<SpikePage> createState() => _SpikePageState();
}

class _SpikePageState extends State<SpikePage> {
  final _log = <String>[];
  bool _advertising = false;

  /// Also appended to %TEMP%\pc1500_ble.log, for reading without the window.
  final _logFile = File('${Directory.systemTemp.path}${Platform.pathSeparator}pc1500_ble.log');

  void _add(String line) {
    final t = TimeOfDay.now().format(context);
    setState(() => _log.insert(0, '$t  $line'));
    _logFile.writeAsStringSync('${DateTime.now().toIso8601String()}  $line\n', mode: FileMode.append);
  }

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) => _start());
  }

  Future<void> _start() async {
    try {
      // On Windows, initialize() only starts looking for the radio and
      // isSupported() is false until it's found; the plugin reports the
      // radio's state once it has one, so wait for that first.
      final radioFound = Completer<void>();
      BlePeripheral.setBleStateChangeCallback((on) {
        _add('Bluetooth ${on ? "on" : "off"}');
        if (!radioFound.isCompleted) radioFound.complete();
      });
      await BlePeripheral.initialize();
      await radioFound.future.timeout(const Duration(seconds: 5), onTimeout: () {});
      if (!await BlePeripheral.isSupported()) {
        _add('No Bluetooth radio found');
        return;
      }
      BlePeripheral.setAdvertisingStatusUpdateCallback((adv, error) {
        setState(() => _advertising = adv);
        _add(adv ? 'Advertising as $advertisedName' : 'Advertising stopped${error != null ? ": $error" : ""}');
      });
      BlePeripheral.setServiceAddedCallback((id, error) =>
          _add(error == null ? 'Service added $id' : 'Service add FAILED: $error'));
      BlePeripheral.setCharacteristicSubscriptionChangeCallback((device, char, subscribed, name) =>
          _add('${name ?? device} ${subscribed ? "subscribed to" : "unsubscribed from"} TX'));
      BlePeripheral.setMtuChangeCallback((device, mtu) => _add('MTU $mtu ($device)'));
      BlePeripheral.setWriteRequestCallback(_onWrite);

      await BlePeripheral.addService(BleService(
        uuid: linkServiceUuid,
        primary: true,
        characteristics: [
          BleCharacteristic(
            uuid: linkRxUuid,
            properties: [CharacteristicProperties.writeWithoutResponse.index, CharacteristicProperties.write.index],
            permissions: [AttributePermissions.writeable.index],
          ),
          BleCharacteristic(
            uuid: linkTxUuid,
            properties: [CharacteristicProperties.notify.index],
            permissions: [AttributePermissions.readable.index],
          ),
        ],
      ));
      await BlePeripheral.startAdvertising(services: [linkServiceUuid], localName: advertisedName);
    } catch (e) {
      _add('Error: $e');
    }
  }

  // Always returns a result: the plugin's Windows side dereferences it
  // without a null check (ble_peripheral 2.4.0), so null crashes the app.
  WriteRequestResult? _onWrite(String device, String char, int offset, Uint8List? value) {
    if (value == null) {
      _add('RX write with no value ($char)');
      return WriteRequestResult();
    }
    final hex = value.map((b) => b.toRadixString(16).padLeft(2, '0')).join(' ');
    final text = latin1.decode(value, allowInvalid: true).replaceAll(RegExp(r'[^\x20-\x7e]'), '.');
    _add('RX ${value.length}B  $hex  "$text"');
    // Echo, so the central sees the TX direction work too.
    BlePeripheral.updateCharacteristic(characteristicId: linkTxUuid, value: value)
        .catchError((Object e) => _add('TX echo failed: $e'));
    return WriteRequestResult();
  }

  @override
  Widget build(BuildContext context) => Scaffold(
        appBar: AppBar(
          title: const Text('PC-1500 BLE — spike'),
          actions: [
            Padding(
              padding: const EdgeInsets.only(right: 16),
              child: Chip(label: Text(_advertising ? 'Advertising' : 'Idle')),
            ),
          ],
        ),
        body: ListView.builder(
          padding: const EdgeInsets.all(12),
          itemCount: _log.length,
          itemBuilder: (_, i) => Text(_log[i], style: const TextStyle(fontFamily: 'Consolas')),
        ),
      );
}
