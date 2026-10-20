// The app's settings (2026-10-20): its name, and where its files live.
// Kept with shared_preferences; the pairings stay in the secure store.
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:shared_preferences/shared_preferences.dart';

class Settings {
  Settings({this.name = defaultName, this.sharedFiles = true, this.filesPath = ''});

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
    );
  }

  Future<void> save() async {
    final p = await SharedPreferences.getInstance();
    await p.setString('name', name);
    await p.setBool('sharedFiles', sharedFiles);
    await p.setString('filesPath', filesPath);
  }

  Settings copy() => Settings(name: name, sharedFiles: sharedFiles, filesPath: filesPath);
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
      ]),
    );
  }
}
