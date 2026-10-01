// PC-1500 BLE app: the feature server a PC-1500 connects to over BLE
// (RP2350/BLE_PROTOCOL.md). It advertises the PC-1500 Link service and
// leaves everything else to the PC-1500: BLPRINT/BLLIST text appears in the
// console, what its CE-150 commands (LPRINT, LINE, ...) draw on the printer's
// paper, and BLSAVE/BLLOAD files live in Documents\PC1500-BLE.
import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:ble_peripheral/ble_peripheral.dart';
import 'package:flutter/material.dart';

import 'link.dart';
import 'plot.dart';

/// The PC-1500 Link service and its two characteristics. Fixed for good:
/// firmware, emulator and app all use these.
const linkServiceUuid = 'c31f0001-92a3-40ab-b63d-7cdb0a37aed0';
const linkRxUuid = 'c31f0002-92a3-40ab-b63d-7cdb0a37aed0'; // PC-1500 -> app, write w/o response
const linkTxUuid = 'c31f0003-92a3-40ab-b63d-7cdb0a37aed0'; // app -> PC-1500, notify

/// Asked for, but Windows advertises under the computer's name instead;
/// the PC-1500 finds the app by the service UUID either way.
const advertisedName = 'PC1500-SRV';

void main() => runApp(const Pc1500BleApp());

class Pc1500BleApp extends StatelessWidget {
  const Pc1500BleApp({super.key});

  @override
  Widget build(BuildContext context) => MaterialApp(
        title: 'PC-1500 BLE',
        theme: ThemeData(colorSchemeSeed: Colors.indigo, useMaterial3: true),
        home: const HomePage(),
      );
}

class HomePage extends StatefulWidget {
  const HomePage({super.key});

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  final _log = <String>[];
  final _console = StringBuffer();
  final _consoleScroll = ScrollController();
  bool _advertising = false;
  String? _connected; // the PC-1500's device id while it's subscribed
  late final Directory _filesDir = Directory(
      '${Platform.environment['USERPROFILE'] ?? Directory.systemTemp.path}${Platform.pathSeparator}Documents'
      '${Platform.pathSeparator}PC1500-BLE');
  final _paper = PlotPaper(); // the CE-150 stand-in's (plot.dart)
  late final LinkServer _link = LinkServer(
    send: _sendFrame,
    filesDir: _filesDir,
    onText: _addText,
    onLog: _add,
    onPlot: _paper.add,
  );

  /// Also appended to %TEMP%\pc1500_ble.log, for reading without the window.
  final _logFile = File('${Directory.systemTemp.path}${Platform.pathSeparator}pc1500_ble.log');

  void _add(String line) {
    if (!mounted) return;
    final t = TimeOfDay.now().format(context);
    setState(() => _log.insert(0, '$t  $line'));
    _logFile.writeAsStringSync('${DateTime.now().toIso8601String()}  $line\n', mode: FileMode.append);
  }

  /// "7E#00 4B" -- a frame's type, number and size, for the log.
  static String _describe(Uint8List f) =>
      f.length < 2 ? '${f.length}B' : '${f[0].toRadixString(16).padLeft(2, '0').toUpperCase()}'
          '#${f[1].toRadixString(16).padLeft(2, '0')} ${f.length}B';

  /// Only a send that fails is logged (every frame was, while the link was
  /// being brought up on 2026-09-27).
  Future<void> _sendFrame(Uint8List frame) async {
    try {
      await BlePeripheral.updateCharacteristic(characteristicId: linkTxUuid, value: frame);
    } catch (e) {
      _add('Send ${_describe(frame)} FAILED: $e');
    }
  }

  /// Console text; a form feed (BLCLS) clears the console first.
  void _addText(String text) {
    final ff = text.lastIndexOf('\f');
    setState(() {
      if (ff >= 0) _console.clear();
      _console.write(ff >= 0 ? text.substring(ff + 1) : text);
    });
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (_consoleScroll.hasClients) _consoleScroll.jumpTo(_consoleScroll.position.maxScrollExtent);
    });
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
        _add(adv ? 'Waiting for a PC-1500' : 'Advertising stopped${error != null ? ": $error" : ""}');
      });
      BlePeripheral.setServiceAddedCallback((id, error) {
        if (error != null) _add('Service add FAILED: $error');
      });
      BlePeripheral.setCharacteristicSubscriptionChangeCallback((device, char, subscribed, name) {
        if (char.toLowerCase() != linkTxUuid) return;
        _link.reset();
        setState(() => _connected = subscribed ? device : null);
        _add(subscribed ? 'PC-1500 connected' : 'PC-1500 disconnected');
      });
      BlePeripheral.setMtuChangeCallback((device, mtu) {
        _link.frameMax = mtu - 3 > 252 ? 252 : mtu - 3;
        _add('MTU $mtu');
      });
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
    if (value != null && char.toLowerCase() == linkRxUuid) {
      _link.handle(value);
    } else {
      _add('Write to $char ignored');
    }
    return WriteRequestResult();
  }

  @override
  Widget build(BuildContext context) {
    final status = _connected != null
        ? 'Connected${_link.peerName != null ? ": ${_link.peerName}" : ""}'
        : (_advertising ? 'Waiting for a PC-1500' : 'Not advertising');
    return Scaffold(
      appBar: AppBar(
        title: const Text('PC-1500 BLE'),
        actions: [
          Padding(padding: const EdgeInsets.only(right: 16), child: Chip(label: Text(status))),
        ],
      ),
      body: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Expanded(
            flex: 3,
            child: Row(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                Expanded(
                  child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
                    Padding(
                      padding: const EdgeInsets.fromLTRB(12, 8, 12, 4),
                      child: Row(children: [
                        const Text('Console', style: TextStyle(fontWeight: FontWeight.bold)),
                        const Spacer(),
                        TextButton(onPressed: () => setState(_console.clear), child: const Text('Clear')),
                      ]),
                    ),
                    Expanded(
                      child: Container(
                        margin: const EdgeInsets.only(left: 12),
                        padding: const EdgeInsets.all(8),
                        decoration: BoxDecoration(
                          color: Theme.of(context).colorScheme.surfaceContainerHighest,
                          borderRadius: BorderRadius.circular(6),
                        ),
                        child: SingleChildScrollView(
                          controller: _consoleScroll,
                          child: SelectableText(_console.toString(),
                              style: const TextStyle(fontFamily: 'Consolas', fontSize: 15)),
                        ),
                      ),
                    ),
                  ]),
                ),
                // The plotter: what the PC-1500's CE-150 commands drew.
                SizedBox(
                  width: 320,
                  child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
                    Padding(
                      padding: const EdgeInsets.fromLTRB(12, 8, 12, 4),
                      child: Row(children: [
                        const Text('Plotter (CE-150)', style: TextStyle(fontWeight: FontWeight.bold)),
                        const Spacer(),
                        TextButton(onPressed: _paper.clear, child: const Text('Clear')),
                      ]),
                    ),
                    Expanded(
                      child: Padding(
                        padding: const EdgeInsets.only(left: 12, right: 12),
                        child: ClipRRect(borderRadius: BorderRadius.circular(6), child: PaperView(paper: _paper)),
                      ),
                    ),
                  ]),
                ),
              ],
            ),
          ),
          Padding(
            padding: const EdgeInsets.fromLTRB(12, 8, 12, 0),
            child: SelectableText('Files: ${_filesDir.path}', style: Theme.of(context).textTheme.bodySmall),
          ),
          const Divider(),
          Expanded(
            flex: 2,
            child: ListView.builder(
              padding: const EdgeInsets.symmetric(horizontal: 12),
              itemCount: _log.length,
              itemBuilder: (_, i) => Text(_log[i], style: const TextStyle(fontFamily: 'Consolas', fontSize: 12)),
            ),
          ),
        ],
      ),
    );
  }
}
