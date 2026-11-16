// The app's settings (2026-10-20): its name, and where its files live;
// the printer and how the plotter's paper is laid out (2026-11-14).
// Kept with shared_preferences; the pairings stay in the secure store.
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:printing/printing.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'hardcopy.dart';

class Settings {
  Settings({
    this.name = defaultName,
    this.sharedFiles = true,
    this.filesPath = '',
    this.printerUrl = '',
    this.plotFit = false,
  });

  static const defaultName = 'PC1500-SRV';

  /// The firmware keeps a peer's name to 16 characters (LINK_NAME_MAX).
  static const nameMax = 16;

  /// This side's name in its HELLO: what the PC-1500 shows on connecting.
  /// iOS and macOS advertise it too; Windows and Android advertise the
  /// computer's or phone's own name.
  String name;

  /// Android: files in the shared Documents/PC1500-BLE (needs "All files
  /// access") rather than the app's own folder.
  bool sharedFiles;

  /// Desktops: the files folder; empty for Documents/PC1500-BLE.
  String filesPath;

  /// Desktops: the printer BLPRTC/BLPRTP print on, by its URL; empty for
  /// the system's default.
  String printerUrl;

  /// BLPRTP: the plotter's paper scaled to the page's width, rather than
  /// actual size.
  bool plotFit;

  /// Printable Latin-1, 1 to 16 characters; null if fine, else why not.
  static String? checkName(String name) {
    if (name.isEmpty) return 'A name is needed';
    if (name.length > nameMax) return 'At most $nameMax characters';
    if (name.runes.any((c) => c < 0x20 || c > 0xFF || (c >= 0x7F && c < 0xA0))) return 'Letters, digits and symbols only';
    return null;
  }

  static Future<Settings> load() async {
    final p = await SharedPreferences.getInstance();
    final name = p.getString('name') ?? defaultName;
    return Settings(
      name: checkName(name) == null ? name : defaultName,
      sharedFiles: p.getBool('sharedFiles') ?? true,
      filesPath: p.getString('filesPath') ?? '',
      printerUrl: p.getString('printerUrl') ?? '',
      plotFit: p.getBool('plotFit') ?? false,
    );
  }

  Future<void> save() async {
    final p = await SharedPreferences.getInstance();
    await p.setString('name', name);
    await p.setBool('sharedFiles', sharedFiles);
    await p.setString('filesPath', filesPath);
    await p.setString('printerUrl', printerUrl);
    await p.setBool('plotFit', plotFit);
  }

  Settings copy() => Settings(
      name: name, sharedFiles: sharedFiles, filesPath: filesPath, printerUrl: printerUrl, plotFit: plotFit);
}

/// The settings screen; pops with the edited Settings, or null if
/// cancelled.
class SettingsPage extends StatefulWidget {
  const SettingsPage({super.key, required this.settings, required this.defaultFilesPath});

  final Settings settings;

  /// What an empty files folder means, to show as the hint.
  final String defaultFilesPath;

  @override
  State<SettingsPage> createState() => _SettingsPageState();
}

class _SettingsPageState extends State<SettingsPage> {
  late final Settings _s = widget.settings.copy();
  late final _name = TextEditingController(text: _s.name);
  late final _path = TextEditingController(text: _s.filesPath);
  String? _nameError;
  List<Printer>? _printers; // desktops, once listed

  @override
  void initState() {
    super.initState();
    if (!printsWithDialog) {
      Printing.listPrinters().then((p) {
        if (mounted) setState(() => _printers = p);
      }, onError: (_) {
        if (mounted) setState(() => _printers = const []);
      });
    }
  }

  @override
  void dispose() {
    _name.dispose();
    _path.dispose();
    super.dispose();
  }

  void _save() {
    final error = Settings.checkName(_name.text.trim());
    if (error != null) return setState(() => _nameError = error);
    _s.name = _name.text.trim();
    _s.filesPath = _path.text.trim();
    Navigator.pop(context, _s);
  }

  @override
  Widget build(BuildContext context) {
    final hint = Theme.of(context).textTheme.bodySmall;
    final desktop = Platform.isWindows || Platform.isMacOS || Platform.isLinux;
    return Scaffold(
      appBar: AppBar(
        title: const Text('Settings'),
        actions: [
          TextButton(onPressed: _save, child: const Text('Save')),
          const SizedBox(width: 8),
        ],
      ),
      body: ListView(padding: const EdgeInsets.all(16), children: [
        TextField(
          controller: _name,
          maxLength: Settings.nameMax,
          decoration: InputDecoration(
            labelText: 'Name',
            helperText: 'What the PC-1500 shows when it connects.',
            errorText: _nameError,
          ),
          onChanged: (_) => setState(() => _nameError = null),
          onSubmitted: (_) => _save(),
        ),
        if (Platform.isAndroid || Platform.isWindows)
          Padding(
            padding: const EdgeInsets.only(top: 4),
            child: Text(
              'BLSCAN lists this ${Platform.isAndroid ? "phone" : "computer"} under its own Bluetooth name, '
              'and BLCON "name" looks for that one.',
              style: hint,
            ),
          ),
        const SizedBox(height: 24),
        Text('Files (BLSAVE / BLLOAD)', style: Theme.of(context).textTheme.titleSmall),
        if (Platform.isAndroid) ...[
          RadioGroup<bool>(
            groupValue: _s.sharedFiles,
            onChanged: (v) => setState(() => _s.sharedFiles = v ?? true),
            child: const Column(children: [
              RadioListTile(
                value: true,
                title: Text('Documents/PC1500-BLE'),
                subtitle: Text('Reachable from file managers and over USB; needs "All files access".'),
              ),
              RadioListTile(
                value: false,
                title: Text("The app's own folder"),
                subtitle: Text('Needs no permission; only this app can reach it.'),
              ),
            ]),
          ),
        ] else if (desktop) ...[
          TextField(
            controller: _path,
            decoration: InputDecoration(
              labelText: 'Folder',
              hintText: widget.defaultFilesPath,
              helperText: 'Empty for the default. Created when the first file is saved.',
              suffixIcon: IconButton(
                tooltip: 'Default',
                icon: const Icon(Icons.restore),
                onPressed: () => setState(_path.clear),
              ),
            ),
          ),
        ] else
          Padding(
            padding: const EdgeInsets.only(top: 8),
            child: Text("In the app's own Documents folder.", style: hint),
          ),
        const SizedBox(height: 24),
        Text('Printing (BLPRTC / BLPRTP)', style: Theme.of(context).textTheme.titleSmall),
        if (printsWithDialog)
          Padding(
            padding: const EdgeInsets.only(top: 8),
            child: Text("The system's print dialog opens to choose the printer.", style: hint),
          )
        else
          _printerChoice(hint),
        RadioGroup<bool>(
          groupValue: _s.plotFit,
          onChanged: (v) => setState(() => _s.plotFit = v ?? false),
          child: const Column(children: [
            RadioListTile(
              value: false,
              title: Text('Plotter paper at actual size'),
              subtitle: Text('58 mm wide, as a CE-150 draws it, down as many pages as it takes.'),
            ),
            RadioListTile(
              value: true,
              title: Text('Plotter paper to the page width'),
              subtitle: Text('Scaled up to fill the page across; a long roll takes more pages.'),
            ),
          ]),
        ),
      ]),
    );
  }

  /// Desktops: the system's default, or one printer by name.
  Widget _printerChoice(TextStyle? hint) {
    final printers = _printers;
    if (printers == null) {
      return Padding(padding: const EdgeInsets.only(top: 8), child: Text('Looking for printers...', style: hint));
    }
    final known = printers.any((p) => p.url == _s.printerUrl);
    return DropdownButtonFormField<String>(
      initialValue: known ? _s.printerUrl : '',
      decoration: const InputDecoration(labelText: 'Printer'),
      items: [
        const DropdownMenuItem(value: '', child: Text("The system's default")),
        for (final p in printers) DropdownMenuItem(value: p.url, child: Text(p.name, overflow: TextOverflow.ellipsis)),
      ],
      onChanged: (v) => setState(() => _s.printerUrl = v ?? ''),
    );
  }
}
