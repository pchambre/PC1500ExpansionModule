// The Link's security (RP2350/BLE_PROTOCOL.md sec.7, 2026-10-03): the
// pairing, the proofs and the sealed frames, byte for byte as the
// firmware's RP2350/link_secure.c makes them (test/secure_test.dart holds
// its interop vectors). Synchronous, so frames are sealed and opened in the
// order they come and go.
import 'dart:math';
import 'dart:typed_data';

import 'package:crypto/crypto.dart' as c;
import 'package:cryptography/cryptography.dart';
import 'package:cryptography/dart.dart';

const idLen = 8, nonceLen = 16, keyLen = 32, pubLen = 32, proofLen = 16;
const headerLen = 4;

/// A sealed frame's extra bytes: its counter and its tag.
const sealOverhead = 20;

final _random = Random.secure();

Uint8List randomBytes(int n) => Uint8List.fromList(List.generate(n, (_) => _random.nextInt(256)));

Uint8List _cat(List<List<int>> parts) {
  final b = BytesBuilder(copy: false);
  for (final p in parts) {
    b.add(p);
  }
  return b.takeBytes();
}

Uint8List _ascii(String s) => Uint8List.fromList(s.codeUnits);

Uint8List sha512(List<List<int>> parts) => Uint8List.fromList(c.sha512.convert(_cat(parts)).bytes);

Uint8List hmacSha512(List<int> key, List<List<int>> parts) =>
    Uint8List.fromList(c.Hmac(c.sha512, key).convert(_cat(parts)).bytes);

/// HKDF-SHA512 (RFC 5869), as Monocypher's crypto_sha512_hkdf.
Uint8List hkdfSha512(List<int> ikm, List<int> salt, List<int> info, int length) {
  final prk = hmacSha512(salt, [ikm]);
  final out = BytesBuilder();
  var t = Uint8List(0);
  for (var i = 1; out.length < length; i++) {
    t = hmacSha512(prk, [t, info, [i]]);
    out.add(t);
  }
  return Uint8List.sublistView(out.takeBytes(), 0, length);
}

/// Constant-time comparison of two proofs.
bool equal16(List<int> a, List<int> b) {
  if (a.length < proofLen || b.length < proofLen) return false;
  var d = 0;
  for (var i = 0; i < proofLen; i++) {
    d |= a[i] ^ b[i];
  }
  return d == 0;
}

// ---- pairing ----

/// An X25519 key pair made from 32 random bytes (ls_keypair).
class PairKeys {
  PairKeys._(this._pair, this.publicKey);
  final SimpleKeyPairData _pair;
  final Uint8List publicKey;

  static Future<PairKeys> fromRandom(List<int> random32) async {
    final pair = await const DartX25519().newKeyPairFromSeed(random32);
    final data = await pair.extract();
    return PairKeys._(data, Uint8List.fromList(data.publicKey.bytes));
  }

  /// The pairing's key (ls_pair_ltk); null for a peer key that gives an
  /// all-zero secret.
  Uint8List? ltk(List<int> peerPublic, List<int> pkC, List<int> pkS, List<int> nC, List<int> nS) {
    final secret = const DartX25519().sharedSecretSync(
      keyPairData: _pair,
      remotePublicKey: SimplePublicKey(peerPublic, type: KeyPairType.x25519),
    ) as SecretKeyData;
    final shared = secret.bytes;
    if (shared.every((b) => b == 0)) return null;
    return hkdfSha512(shared, _cat([nC, nS]), _cat([_ascii('PC1500 pair'), pkC, pkS]), keyLen);
  }
}

Uint8List pairCommit(List<int> pkS, List<int> pkC, List<int> nS) =>
    Uint8List.sublistView(sha512([_ascii('PC1500 commit'), pkS, pkC, nS]), 0, proofLen);

/// The six-digit code both people compare.
int pairCode(List<int> pkC, List<int> pkS, List<int> nC, List<int> nS) {
  final h = sha512([_ascii('PC1500 code'), pkC, pkS, nC, nS]);
  return ((h[0] << 24) | (h[1] << 16) | (h[2] << 8) | h[3]) % 1000000;
}

String codeText(int code) => code.toString().padLeft(6, '0');

Uint8List _mac16(List<int> key, String label, String role, List<List<int>> parts) =>
    Uint8List.sublistView(hmacSha512(key, [_ascii(label), _ascii(role), ...parts]), 0, proofLen);

/// role 'C' (the connector) or 'S' (the advertiser).
Uint8List pairConfirm(List<int> ltk, String role, List<int> idC, List<int> idS) =>
    _mac16(ltk, 'PC1500 confirm ', role, [idC, idS]);

// ---- every link ----

Uint8List authProof(List<int> ltk, String role, List<int> nonceC, List<int> nonceS, List<int> idC, List<int> idS) =>
    _mac16(ltk, 'PC1500 auth ', role, [nonceC, nonceS, idC, idS]);

/// A link's keys and counters once it has authenticated: every frame
/// sealed with ChaCha20-Poly1305 (`[type][seq][len][counter u32 LE][ct][tag]`).
class Session {
  Session.start(List<int> ltk, List<int> nonceC, List<int> nonceS, {required bool connector}) {
    final okm = hkdfSha512(ltk, _cat([nonceC, nonceS]), _ascii('PC1500 session'), 64);
    final c2s = SecretKeyData(Uint8List.sublistView(okm, 0, 32));
    final s2c = SecretKeyData(Uint8List.sublistView(okm, 32, 64));
    _tx = connector ? c2s : s2c;
    _rx = connector ? s2c : c2s;
  }

  static const _aead = DartChacha20.poly1305Aead();
  late final SecretKeyData _tx, _rx;
  int _txCounter = 0;
  bool _rxAny = false;
  int _rxHigh = 0, _rxSeen = 0;

  static Uint8List _nonce(int counter) => Uint8List(12)..buffer.asByteData().setUint32(4, counter, Endian.little);

  static Uint8List _aad(int type, int seq, int counter) =>
      Uint8List(6)
        ..[0] = type
        ..[1] = seq
        ..buffer.asByteData().setUint32(2, counter, Endian.little);

  /// A plain frame sealed; null once the counter has run out.
  Uint8List? seal(Uint8List frame) {
    if (frame.length < headerLen || _txCounter == 0xFFFFFFFF) return null;
    final counter = _txCounter++;
    final n = frame.length - headerLen;
    final box = _aead.encryptSync(Uint8List.sublistView(frame, headerLen),
        secretKey: _tx, nonce: _nonce(counter), aad: _aad(frame[0], frame[1], counter));
    final len = n + sealOverhead;
    final out = BytesBuilder(copy: false)
      ..add([frame[0], frame[1], len & 0xFF, len >> 8])
      ..add(Uint8List(4)..buffer.asByteData().setUint32(0, counter, Endian.little))
      ..add(box.cipherText)
      ..add(box.mac.bytes);
    return out.takeBytes();
  }

  /// A sealed frame opened; null if it fails, is replayed or is too late
  /// (a window of 32).
  Uint8List? open(Uint8List frame) {
    if (frame.length < headerLen + sealOverhead) return null;
    final n = frame.length - headerLen - sealOverhead;
    final counter = ByteData.sublistView(frame, 4, 8).getUint32(0, Endian.little);
    var diff = 0;
    if (_rxAny && counter <= _rxHigh) {
      diff = _rxHigh - counter;
      if (diff >= 32 || (_rxSeen & (1 << diff)) != 0) return null;
    }
    List<int> plain;
    try {
      plain = _aead.decryptSync(
          SecretBox(Uint8List.sublistView(frame, 8, 8 + n),
              nonce: _nonce(counter), mac: Mac(Uint8List.sublistView(frame, 8 + n))),
          secretKey: _rx,
          aad: _aad(frame[0], frame[1], counter));
    } on SecretBoxAuthenticationError {
      return null;
    }
    if (!_rxAny || counter > _rxHigh) {
      final shift = _rxAny ? counter - _rxHigh : 32;
      _rxSeen = shift >= 32 ? 1 : ((_rxSeen << shift) | 1) & 0xFFFFFFFF;
      _rxHigh = counter;
      _rxAny = true;
    } else {
      _rxSeen |= 1 << diff;
    }
    return Uint8List.fromList([frame[0], frame[1], n & 0xFF, n >> 8, ...plain]);
  }
}

// ---- what's kept ----

class Pairing {
  Pairing(this.id, this.name, this.ltk);
  final Uint8List id;
  final String name;
  final Uint8List ltk;

  String get idHex => id.map((b) => b.toRadixString(16).padLeft(2, '0')).join().toUpperCase();
}

/// This side's identity and the pairings it keeps. pairing_store.dart's
/// keeps them in the platform's secure storage; MemoryPairingStore is for
/// the tests.
abstract class PairingStore {
  Uint8List get id;
  List<Pairing> get all;
  Pairing? find(List<int> id) {
    for (final p in all) {
      if (_sameId(p.id, id)) return p;
    }
    return null;
  }

  Future<void> add(Pairing p);
  Future<void> forget(List<int> id);

  static bool _sameId(List<int> a, List<int> b) {
    if (a.length != b.length) return false;
    for (var i = 0; i < a.length; i++) {
      if (a[i] != b[i]) return false;
    }
    return true;
  }
}

class MemoryPairingStore extends PairingStore {
  MemoryPairingStore([Uint8List? id]) : id = id ?? randomBytes(idLen);
  @override
  final Uint8List id;
  final _pairs = <Pairing>[];
  @override
  List<Pairing> get all => List.unmodifiable(_pairs);
  @override
  Future<void> add(Pairing p) async {
    _pairs.removeWhere((q) => PairingStore._sameId(q.id, p.id));
    _pairs.add(p);
  }

  @override
  Future<void> forget(List<int> id) async => _pairs.removeWhere((q) => PairingStore._sameId(q.id, id));
}
