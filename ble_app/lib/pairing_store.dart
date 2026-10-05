// Where the app keeps its Link identity and the PC-1500s paired with it
// (RP2350/BLE_PROTOCOL.md sec.7, 2026-10-03): the platform's own secret
// store, through flutter_secure_storage -- DPAPI on Windows, the Keychain
// on iOS and macOS (this device only, never synced), the Android Keystore,
// libsecret on Linux. Never the files folder, which a paired PC-1500 can
// read. Losing it only means pairing again.
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_secure_storage/flutter_secure_storage.dart';

import 'secure.dart';

class SecurePairingStore extends PairingStore {
  SecurePairingStore._(this.id, this._pairs);

  static const _key = 'pc1500_link_pairings';
  static const _storage = FlutterSecureStorage(
    iOptions: IOSOptions(accessibility: KeychainAccessibility.first_unlock_this_device),
    mOptions: MacOsOptions(accessibility: KeychainAccessibility.first_unlock_this_device),
  );

  @override
  final Uint8List id;
  final List<Pairing> _pairs;

  @override
  List<Pairing> get all => List.unmodifiable(_pairs);

  /// What was kept, or a new identity (kept from now on).
  static Future<SecurePairingStore> load() async {
    String? raw;
    try {
      raw = await _storage.read(key: _key);
    } catch (_) {
      raw = null; // unreadable (a restored backup, a changed key): start again
    }
    if (raw != null) {
      try {
        final j = jsonDecode(raw) as Map<String, dynamic>;
        final pairs = [
          for (final p in j['pairs'] as List<dynamic>)
            Pairing(_unhex(p['id'] as String), p['name'] as String, _unhex(p['ltk'] as String)),
        ];
        return SecurePairingStore._(_unhex(j['id'] as String), pairs);
      } catch (_) {}
    }
    final store = SecurePairingStore._(randomBytes(idLen), []);
    await store._save();
    return store;
  }

  @override
  Future<void> add(Pairing p) async {
    _pairs.removeWhere((q) => q.idHex == p.idHex);
    _pairs.add(p);
    await _save();
  }

  @override
  Future<void> forget(List<int> id) async {
    final hex = _hex(id);
    _pairs.removeWhere((q) => q.idHex == hex);
    await _save();
  }

  Future<void> _save() => _storage.write(
      key: _key,
      value: jsonEncode({
        'id': _hex(id),
        'pairs': [
          for (final p in _pairs) {'id': p.idHex, 'name': p.name, 'ltk': _hex(p.ltk)},
        ],
      }));

  static String _hex(List<int> b) => b.map((x) => x.toRadixString(16).padLeft(2, '0')).join().toUpperCase();

  static Uint8List _unhex(String s) =>
      Uint8List.fromList([for (var i = 0; i + 1 < s.length; i += 2) int.parse(s.substring(i, i + 2), radix: 16)]);
}
