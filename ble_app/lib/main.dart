// PC1500BLEHost: the feature server a PC-1500 connects to over BLE
// (RP2350/BLE_PROTOCOL.md). It advertises the PC-1500 Link service and
// leaves everything else to the PC-1500: BLPRINT/BLLIST text appears in the
// console, what its CE-150 commands (LPRINT, LINE, ...) draw on the printer's
// paper, and BLSAVE/BLLOAD files live in Documents\PC1500-BLE (the app's
// own Documents folder on iOS). A PC-1500 pairs with it once
// (BLPAIR, the code accepted here); only paired ones get in.
import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:ble_peripheral/ble_peripheral.dart';
import 'package:flutter/material.dart';
import 'package:path_provider/path_provider.dart';
import 'package:permission_handler/permission_handler.dart';

import 'link.dart';
import 'pairing_store.dart';
import 'plot.dart';
import 'screen.dart';
import 'secure.dart';
import 'settings.dart';
import 'split.dart';

/// The PC-1500 Link service and its two characteristics. Fixed for good:
/// firmware, emulator and app all use these.
const linkServiceUuid = 'c31f0001-92a3-40ab-b63d-7cdb0a37aed0';
const linkRxUuid = 'c31f0002-92a3-40ab-b63d-7cdb0a37aed0'; // PC-1500 -> app, write w/o response
const linkTxUuid = 'c31f0003-92a3-40ab-b63d-7cdb0a37aed0'; // app -> PC-1500, notify

void main() => runApp(const Pc1500BleApp());

class Pc1500BleApp extends StatelessWidget {
  const Pc1500BleApp({super.key});

  @override
  Widget build(BuildContext context) => MaterialApp(
        title: 'PC1500BLEHost',
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
  Directory? _filesDir;
  final _paper = PlotPaper(); // the CE-150 stand-in's (plot.dart)
  final _screen = ScreenModel(); // the PC-1500's screen text (screen.dart)
  PairingStore? _pairings;
  LinkServer? _link; // once the files folder and the pairings are known
  bool _pairDialog = false;
  Settings _settings = Settings();

  /// The files folder the settings ask for: on Android the shared
  /// Documents/PC1500-BLE (given the storage permission) or the app's own;
  /// on desktops the one set, else Documents/PC1500-BLE; on iOS the app's.
  Future<Directory> _findFilesDir() async {
    if (Platform.isAndroid && _settings.sharedFiles) {
      // "All files access" on Android 11+, the storage permission before
      // that; only one of the two applies, the other reports restricted.
      final all = await Permission.manageExternalStorage.request();
      final legacy = all.isGranted ? all : await Permission.storage.request();
      if (all.isGranted || legacy.isGranted) {
        return Directory('/storage/emulated/0/Documents/PC1500-BLE');
      }
      _add('Files access refused -- files stay inside the app');
    }
    if (!Platform.isAndroid && !Platform.isIOS && _settings.filesPath.isNotEmpty) {
      return Directory(_settings.filesPath);
    }
    return _defaultFilesDir();
  }

  /// Documents/PC1500-BLE: the user's on desktops, the app's own on phones.
  static Future<Directory> _defaultFilesDir() async {
    Directory base;
    try {
      base = await getApplicationDocumentsDirectory();
    } catch (_) {
      base = Directory.systemTemp;
    }
    return Directory('${base.path}${Platform.pathSeparator}PC1500-BLE');
  }

  /// A PC-1500's BLPAIR: its code, for this side's user to compare with the
  /// one on its screen. A null code takes the question away.
  void _onPairRequest(String? code, String? peer) {
    if (!mounted) return;
    if (code == null) {
      if (_pairDialog) {
        _pairDialog = false;
        Navigator.of(context, rootNavigator: true).pop();
      }
      return;
    }
    _pairDialog = true;
    showDialog<bool>(
      context: context,
      barrierDismissible: false,
      builder: (ctx) => AlertDialog(
        title: Text('Pair with ${peer ?? "a PC-1500"}?'),
        content: Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.start, children: [
          const Text('Accept only if the PC-1500 shows the same code:'),
          const SizedBox(height: 12),
          Text(code, style: const TextStyle(fontFamily: 'Consolas', fontSize: 32, letterSpacing: 4)),
          const SizedBox(height: 12),
          const Text('Once paired, it can read and write the files folder.'),
        ]),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('Reject')),
          FilledButton(onPressed: () => Navigator.pop(ctx, true), child: const Text('Accept')),
        ],
      ),
    ).then((ok) {
      _pairDialog = false;
      if (ok != null) _link?.answerPairing(ok);
      if (mounted) setState(() {});
    });
  }

  /// The PC-1500s paired with this app, each with Forget.
  void _showPairings() {
    final store = _pairings;
    if (store == null) return;
    showDialog<void>(
      context: context,
      builder: (ctx) => StatefulBuilder(
        builder: (ctx, refresh) => AlertDialog(
          title: const Text('Paired PC-1500s'),
          content: SizedBox(
            width: 360,
            child: store.all.isEmpty
                ? const Text('None yet. On the PC-1500, BLPAIR pairs with this app.')
                : Column(mainAxisSize: MainAxisSize.min, children: [
                    for (final p in store.all)
                      ListTile(
                        title: Text(p.name),
                        subtitle: Text(p.idHex, style: const TextStyle(fontFamily: 'Consolas', fontSize: 12)),
                        trailing: TextButton(
                          onPressed: () async {
                            await store.forget(p.id);
                            _add('Forgot ${p.name}');
                            refresh(() {});
                          },
                          child: const Text('Forget'),
                        ),
                      ),
                  ]),
          ),
          actions: [TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Close'))],
        ),
      ),
    );
  }

  /// Also appended to %TEMP%\PC1500BLEHost.log, for reading without the window.
  final _logFile = File('${Directory.systemTemp.path}${Platform.pathSeparator}PC1500BLEHost.log');

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
      _settings = await Settings.load();
      final filesDir = await _findFilesDir();
      final pairings = await SecurePairingStore.load();
      setState(() {
        _filesDir = filesDir;
        _pairings = pairings;
        _link = LinkServer(
          send: _sendFrame,
          filesDir: filesDir,
          onText: _addText,
          onLog: _add,
          onPlot: _paper.add,
          onDisplay: _screen.add,
          pairings: pairings,
          onPairRequest: _onPairRequest,
          name: _settings.name,
        );
      });
      _add('${pairings.all.length} paired PC-1500(s)');
      // On Windows, initialize() only starts looking for the radio and
      // isSupported() is false until it's found; the plugin reports the
      // radio's state once it has one, so wait for that first.
      final radioFound = Completer<void>();
      BlePeripheral.setBleStateChangeCallback((on) {
        _add('Bluetooth ${on ? "on" : "off"}');
        if (!radioFound.isCompleted) radioFound.complete();
      });
      // Android 12+ grants advertising and GATT serving only at runtime
      // ("Nearby devices"); without it the plugin throws SecurityException.
      if (Platform.isAndroid) {
        final granted = await [Permission.bluetoothAdvertise, Permission.bluetoothConnect].request();
        if (granted.values.any((s) => !s.isGranted)) {
          _add('Nearby devices permission refused -- allow it in Settings and restart the app');
          return;
        }
      }
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
        _link?.reset();
        setState(() => _connected = subscribed ? device : null);
        _add(subscribed ? 'PC-1500 connected' : 'PC-1500 disconnected');
      });
      BlePeripheral.setMtuChangeCallback((device, mtu) {
        _link?.frameMax = mtu - 3 > 252 ? 252 : mtu - 3;
        _add('MTU $mtu');
      });
      BlePeripheral.setWriteRequestCallback(_onWrite);
      // Android stops advertising when a central connects, and says nothing;
      // after it goes, advertise again or no PC-1500 finds us.
      if (Platform.isAndroid) {
        BlePeripheral.setConnectionStateChangeCallback((device, connected) async {
          if (connected) return;
          try {
            await BlePeripheral.stopAdvertising();
            await BlePeripheral.startAdvertising(services: [linkServiceUuid], localName: _settings.name);
          } catch (e) {
            _add('Advertising again FAILED: $e');
          }
        });
      }

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
      await BlePeripheral.startAdvertising(services: [linkServiceUuid], localName: _settings.name);
    } catch (e) {
      _add('Error: $e');
    }
  }

  // Always returns a result: the plugin's Windows side dereferences it
  // without a null check (ble_peripheral 2.4.0), so null crashes the app.
  WriteRequestResult? _onWrite(String device, String char, int offset, Uint8List? value) {
    if (value != null && char.toLowerCase() == linkRxUuid) {
      _link?.handle(value);
    } else {
      _add('Write to $char ignored');
    }
    return WriteRequestResult();
  }

  @override
  Widget build(BuildContext context) {
    final link = _link;
    final status = _connected != null
        ? '${link?.authenticated == true ? "Connected" : "Connecting"}'
            '${link?.peerName != null ? ": ${link!.peerName}" : ""}'
        : (_advertising ? 'Waiting for a PC-1500' : 'Not advertising');
    // Narrow (a phone): one pane at a time, in tabs. Wide: all of them,
    // with draggable dividers between.
    final narrow = MediaQuery.sizeOf(context).width < 700;
    final paired = _pairings?.all.length ?? 0;
    final actions = [
      if (narrow)
        IconButton(
          onPressed: _pairings == null ? null : _showPairings,
          tooltip: 'Paired ($paired)',
          icon: Badge(label: Text('$paired'), child: const Icon(Icons.link)),
        )
      else
        TextButton.icon(
          onPressed: _pairings == null ? null : _showPairings,
          icon: const Icon(Icons.link),
          label: Text('Paired ($paired)'),
        ),
      IconButton(
        onPressed: _link == null ? null : _openSettings,
        tooltip: 'Settings',
        icon: const Icon(Icons.settings),
      ),
      const SizedBox(width: 8),
      Padding(padding: const EdgeInsets.only(right: 16), child: Chip(label: Text(status))),
    ];
    if (narrow) {
      return DefaultTabController(
        length: 4,
        child: Scaffold(
          appBar: AppBar(
            title: const Text('PC1500BLEHost'),
            actions: actions,
            bottom: const TabBar(tabs: [Tab(text: 'Screen'), Tab(text: 'Console'), Tab(text: 'Plotter'), Tab(text: 'Log')]),
          ),
          body: TabBarView(
            physics: const NeverScrollableScrollPhysics(), // the plotter pans by dragging
            children: [_screenPane(), _consolePane(), _plotterPane(), _logPane()],
          ),
        ),
      );
    }
    return Scaffold(
      appBar: AppBar(title: const Text('PC1500BLEHost'), actions: actions),
      // The plotter, full height on the left; the screen, the console and
      // the log stacked to its right.
      body: SplitPane(
        initial: 0.3,
        minSize: 160,
        first: _plotterPane(),
        second: SplitPane(
          vertical: true,
          initial: 0.5,
          first: _screenPane(),
          second: SplitPane(vertical: true, initial: 0.5, first: _consolePane(), second: _logPane()),
        ),
      ),
    );
  }

  /// The settings screen; what's saved applies at once: the name from the
  /// next HELLO (and advertisement), the files folder from the next transfer.
  Future<void> _openSettings() async {
    final link = _link;
    if (link == null) return;
    final defaultDir = await _defaultFilesDir();
    if (!mounted) return;
    final s = await Navigator.push<Settings>(
      context,
      MaterialPageRoute(builder: (_) => SettingsPage(settings: _settings, defaultFilesPath: defaultDir.path)),
    );
    if (s == null) return;
    final renamed = s.name != _settings.name;
    final moved = s.sharedFiles != _settings.sharedFiles || s.filesPath != _settings.filesPath;
    _settings = s;
    await s.save();
    link.name = s.name;
    if (moved) {
      final dir = await _findFilesDir();
      link.filesDir = dir;
      setState(() => _filesDir = dir);
      _add('Files: ${dir.path}');
    }
    if (renamed) {
      _add('Name: ${s.name}');
      if (_advertising) {
        try {
          await BlePeripheral.stopAdvertising();
          await BlePeripheral.startAdvertising(services: [linkServiceUuid], localName: s.name);
        } catch (e) {
          _add('Advertising again FAILED: $e');
        }
      }
    }
  }

  static const _mono =TextStyle(fontFamily: 'Consolas', fontFamilyFallback: ['monospace']);

  Widget _pane(String title, VoidCallback onClear, Widget body) {
    return Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
      Padding(
        padding: const EdgeInsets.fromLTRB(12, 8, 12, 4),
        child: Row(children: [
          Text(title, style: const TextStyle(fontWeight: FontWeight.bold)),
          const Spacer(),
          TextButton(onPressed: onClear, child: const Text('Clear')),
        ]),
      ),
      Expanded(
        child: Padding(
          padding: const EdgeInsets.fromLTRB(12, 0, 12, 12),
          child: ClipRRect(borderRadius: BorderRadius.circular(6), child: body),
        ),
      ),
    ]);
  }

  /// What the PC-1500's CE-150 commands drew.
  Widget _plotterPane() => _pane('Plotter (CE-150)', _paper.clear, PaperView(paper: _paper));

  /// The PC-1500's own screen, line by line (a PC1500_TEXT_SNOOP dongle's
  /// DISPLAY frames).
  Widget _screenPane() => _pane('Screen', _screen.clear, ScreenView(screen: _screen));

  Widget _consolePane() => _pane(
        'Console',
        () => setState(_console.clear),
        Container(
          padding: const EdgeInsets.all(8),
          color: Theme.of(context).colorScheme.surfaceContainerHighest,
          child: SingleChildScrollView(
            controller: _consoleScroll,
            child: SelectableText(_console.toString(), style: _mono.copyWith(fontSize: 15)),
          ),
        ),
      );

  /// The files folder and the log.
  Widget _logPane() => _pane(
        'Log',
        () => setState(_log.clear),
        Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
          SelectableText('Files: ${_filesDir?.path ?? "..."}', style: Theme.of(context).textTheme.bodySmall),
          const Divider(),
          Expanded(
            child: ListView.builder(
              itemCount: _log.length,
              itemBuilder: (_, i) => Text(_log[i], style: _mono.copyWith(fontSize: 12)),
            ),
          ),
        ]),
      );
}
