// secure.dart against the firmware's link_secure.c: the same inputs must
// give the same bytes (the vectors are link_secure.c's own, 2026-10-03).
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:pc1500_ble/secure.dart';

Uint8List seq(int n, int first) => Uint8List.fromList(List.generate(n, (i) => (first + i) & 0xFF));
String hex(List<int> b) => b.map((x) => x.toRadixString(16).padLeft(2, '0')).join();

void main() {
  final r1 = seq(32, 1), r2 = seq(32, 0x80);
  final nc = seq(16, 0x10), ns = seq(16, 0x40);
  final idc = seq(8, 0xC0), ids = seq(8, 0xD0);

  test('pairing and proofs match the firmware', () async {
    final c = await PairKeys.fromRandom(r1), s = await PairKeys.fromRandom(r2);
    expect(hex(c.publicKey), '07a37cbc142093c8b755dc1b10e86cb426374ad16aa853ed0bdfc0b2b86d1c7c');
    expect(hex(s.publicKey), '493e82fc74464a59268817623d2053c5eb8e2cc4a988b4fee179ec6b010d531d');
    expect(hex(pairCommit(s.publicKey, c.publicKey, ns)), '351497237f890e217f61989ac3c84acf');
    expect(pairCode(c.publicKey, s.publicKey, nc, ns), 104145);
    final ltk = c.ltk(s.publicKey, c.publicKey, s.publicKey, nc, ns)!;
    expect(hex(ltk), '99c852a5b9c55413199fa0bf3d7e9f58205fec37bd4a292bac53ee733a7d1a21');
    expect(hex(s.ltk(c.publicKey, c.publicKey, s.publicKey, nc, ns)!), hex(ltk));
    expect(hex(pairConfirm(ltk, 'C', idc, ids)), '22eeaeb2dd7f399ee28948d1e89e0695');
    expect(hex(pairConfirm(ltk, 'S', idc, ids)), '48e969be51626bf7e91c8d2fda324ad9');
    expect(hex(authProof(ltk, 'C', nc, ns, idc, ids)), '47de5ec682f0a9fbc0f412d611f75d68');
    expect(hex(authProof(ltk, 'S', nc, ns, idc, ids)), '11779da39f7013cfcbf9755728013c74');
  });

  test('sealed frames match the firmware, and open only once', () async {
    final c = await PairKeys.fromRandom(r1), s = await PairKeys.fromRandom(r2);
    final ltk = c.ltk(s.publicKey, c.publicKey, s.publicKey, nc, ns)!;
    final frame = Uint8List.fromList([0x10, 7, 6, 0, 0, ...'HELLO'.codeUnits]);
    final conn = Session.start(ltk, nc, ns, connector: true);
    final adv = Session.start(ltk, nc, ns, connector: false);
    final c2s = conn.seal(frame)!;
    expect(hex(c2s), '10071a00000000003c719106c9950669e198e5fe7633da0f8d9f02e0e3de');
    final s2c = adv.seal(frame)!;
    expect(hex(s2c), '10071a0000000000940e7933119f3890b0381b4102c04e29c5eb8ac35764');
    expect(adv.open(c2s), frame);
    expect(adv.open(c2s), isNull); // replayed
    final tampered = Uint8List.fromList(s2c)..[9] ^= 1;
    expect(conn.open(tampered), isNull);
    expect(conn.open(s2c), frame);
    // late but within the window: accepted once
    final a = conn.seal(frame)!, b = conn.seal(frame)!;
    expect(adv.open(b), frame);
    expect(adv.open(a), frame);
    expect(adv.open(a), isNull);
  });

  test('hkdf matches RFC 5869-style use (64 bytes)', () {
    expect(hkdfSha512([1, 2, 3], [4], [5], 64).length, 64);
  });
}
