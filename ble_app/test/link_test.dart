import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:pc1500_ble/link.dart';
import 'package:pc1500_ble/main.dart';
import 'package:pc1500_ble/plot.dart';
import 'package:pc1500_ble/secure.dart';

/// Plays the PC-1500 against a LinkServer: sends frames, and ACKs every
/// frame the server sends that needs an answer. pair() and hello() do
/// sec.7's pairing and authentication as the firmware's BLPAIR and BLCON do;
/// after hello() every frame both ways is sealed.
class FakePc {
  FakePc(Directory dir, {bool plots = true, PairingStore? store}) {
    serverStore = store ?? MemoryPairingStore();
    server = LinkServer(
        send: _fromServer,
        filesDir: dir,
        onText: text.write,
        onLog: (_) {},
        onPlot: plots ? paper.add : null,
        pairings: serverStore,
        onPairRequest: (code, _) => asked = code);
    server.frameMax = 64;
  }

  late final LinkServer server;
  late final PairingStore serverStore;
  final paper = PlotPaper();
  final text = StringBuffer();
  final answers = <List<int>>[]; // [type, seq, code] of ACK/ERR the server sent
  final payloads = <Uint8List>[]; // each ACK/ERR's payload
  final frames = <Uint8List>[]; // everything else the server sent
  int _seq = 0;
  String? asked; // the code the server's user was shown

  final id = Uint8List.fromList(List.generate(idLen, (i) => 0xC0 + i));
  Uint8List? ltk, serverId;
  Session? session;
  int dropped = 0;

  Future<void> _fromServer(Uint8List f) async {
    if (session != null) {
      final plain = session!.open(f);
      if (plain == null) {
        dropped++;
        return;
      }
      f = plain;
    }
    if (f[0] == T.ack || f[0] == T.err) {
      answers.add([f[0], f[1], f[0] == T.err && f.length > 4 ? f[4] : 0]); // an ACK's payload isn't a code
      payloads.add(Uint8List.sublistView(f, 4));
    } else {
      frames.add(f);
      server.handle(_out(Uint8List.fromList([T.ack, f[1], 0, 0])));
    }
  }

  Uint8List _out(Uint8List f) => session == null ? f : session!.seal(f)!;

  /// Sends a frame; returns the server's answer as [type, code].
  Future<List<int>> send(int type, List<int> payload) async {
    final seq = _seq++ & 0xFF;
    final before = answers.length;
    server.handle(_out(Uint8List.fromList([type, seq, payload.length & 0xFF, payload.length >> 8, ...payload])));
    for (var i = 0; i < 500 && answers.length == before; i++) {
      await Future<void>.delayed(const Duration(milliseconds: 1));
    }
    final a = answers.length > before ? answers[before] : [0, 0, -1];
    expect(a[1], seq);
    return [a[0], a[2]];
  }

  Uint8List get lastPayload => payloads.last;

  Future<void> settle() => Future<void>.delayed(const Duration(milliseconds: 20));

  /// HELLO both ways, then AUTH if the server knows us (and `auth`).
  /// Returns the server's `known`. `reconnect`: on a new link, as BLCON
  /// after BLDISC (the app resets on every subscription).
  Future<bool> hello({bool auth = true, bool reconnect = false}) async {
    if (reconnect) server.reset();
    session = null;
    final nonceC = randomBytes(nonceLen);
    frames.clear();
    expect(await send(T.hello, [2, 1, ...str8('PC-1500'), ...id, ...nonceC]), [T.ack, 0]);
    await settle();
    final h = frames.single;
    expect(h[0], T.hello);
    final n = h[6];
    final at = 7 + n;
    serverId = Uint8List.fromList(h.sublist(at, at + idLen));
    final nonceS = h.sublist(at + idLen, at + idLen + nonceLen);
    final known = h[at + idLen + nonceLen] == 1;
    if (!auth || !known) return known;
    final proofS = h.sublist(at + idLen + nonceLen + 1);
    expect(equal16(proofS, authProof(ltk!, 'S', nonceC, nonceS, id, serverId!)), isTrue);
    expect(await send(T.auth, authProof(ltk!, 'C', nonceC, nonceS, id, serverId!)), [T.ack, 0]);
    session = Session.start(ltk!, nonceC, nonceS, connector: true);
    return true;
  }

  /// BLPAIR: keys, nonces, the code, both answers. Returns the last
  /// PAIR_CONFIRM's answer payload (or [type, code] on an error).
  Future<List<int>> pair({bool serverSays = true, bool tamper = false}) async {
    await hello(auth: false);
    final keys = await PairKeys.fromRandom(randomBytes(32));
    expect(await send(T.pairStart, keys.publicKey), [T.ack, 0]);
    final pkS = lastPayload.sublist(0, pubLen), commit = lastPayload.sublist(pubLen);
    final nC = randomBytes(nonceLen);
    expect(await send(T.pairNonce, nC), [T.ack, 0]);
    final nS = lastPayload;
    expect(equal16(commit, pairCommit(pkS, keys.publicKey, nS)), isTrue);
    final key = keys.ltk(pkS, keys.publicKey, pkS, nC, nS)!;
    expect(asked, codeText(pairCode(keys.publicKey, pkS, nC, nS))); // the same code both sides
    final mac = pairConfirm(tamper ? randomBytes(keyLen) : key, 'C', id, serverId!);
    expect(await send(T.pairConfirm, [1, ...mac]), [T.err, E.busy]); // the server's user hasn't answered
    server.answerPairing(serverSays);
    expect(asked, isNull);
    final r = await send(T.pairConfirm, [1, ...mac]);
    if (r[0] != T.ack) return r;
    final answer = lastPayload;
    if (answer[0] == 1) {
      expect(equal16(answer.sublist(1), pairConfirm(key, 'S', id, serverId!)), isTrue);
      ltk = key;
    }
    return answer;
  }

  /// Paired and authenticated, as after BLPAIR then BLCON.
  Future<void> link() async {
    expect((await pair())[0], 1);
    expect(await hello(), isTrue);
  }
}

List<int> str8(String s) => [s.length, ...latin1.encode(s)];
List<int> u32(int v) => [v & 0xFF, v >> 8 & 0xFF, v >> 16 & 0xFF, v >> 24 & 0xFF];

void main() {
  late Directory dir;
  late FakePc pc;
  setUp(() async {
    dir = Directory.systemTemp.createTempSync('link_test');
    pc = FakePc(dir);
    await pc.link();
  });
  tearDown(() => dir.deleteSync(recursive: true));

  test('Link UUIDs share one base', () {
    expect(linkRxUuid.substring(8), linkServiceUuid.substring(8));
    expect(linkTxUuid.substring(8), linkServiceUuid.substring(8));
  });

  test('HELLO is answered with the server\'s own HELLO, its name and identity', () async {
    expect(await pc.hello(reconnect: true), isTrue);
    expect(pc.server.peerName, 'PC-1500');
    expect(pc.server.authenticated, isTrue);
    final h = pc.frames.single;
    expect(h[4], protocolVersion);
    expect(latin1.decode(h.sublist(7, 7 + h[6])), 'PC1500-SRV');
    expect(pc.serverId, pc.serverStore.id);
  });

  test('an older PC-1500 (version 1) is refused', () async {
    final old = FakePc(dir);
    expect(await old.send(T.hello, [1, 1, ...str8('PC-1500')]), [T.err, E.unsupported]);
  });

  test('unpaired: HELLO says so, and nothing but pairing is answered', () async {
    final stranger = FakePc(dir);
    expect(await stranger.hello(), isFalse);
    expect(await stranger.send(T.text, [0, ...latin1.encode('HI\r')]), [T.err, E.notPaired]);
    expect(await stranger.send(T.fileGet, [0, ...str8('P')]), [T.err, E.notPaired]);
    expect(await stranger.send(T.auth, List.filled(proofLen, 0)), [T.err, E.notPaired]);
    expect(stranger.text.toString(), isEmpty);
  });

  test('pairing: refused by this side\'s user, or with the wrong key, keeps nothing', () async {
    final other = FakePc(dir);
    expect(await other.pair(serverSays: false), [0]);
    expect(other.serverStore.all, isEmpty);
    expect(await other.pair(tamper: true), [T.err, E.authFailed]);
    expect(other.serverStore.all, isEmpty);
    expect(await other.pair(), hasLength(1 + proofLen));
    expect(other.serverStore.all.single.name, 'PC-1500');
  });

  test('a forged proof fails; a forgotten pairing is unpaired', () async {
    pc.ltk = randomBytes(keyLen); // not the key the server holds
    pc.server.reset();
    pc.session = null;
    final nonceC = randomBytes(nonceLen);
    pc.frames.clear();
    expect(await pc.send(T.hello, [2, 1, ...str8('PC-1500'), ...pc.id, ...nonceC]), [T.ack, 0]);
    await pc.settle();
    expect(await pc.send(T.auth, List.filled(proofLen, 7)), [T.err, E.authFailed]);
    await pc.serverStore.forget(pc.id);
    expect(await pc.hello(reconnect: true), isFalse);
  });

  test('sealed: a tampered or replayed frame is dropped unanswered', () async {
    final f = pc.session!.seal(Uint8List.fromList([T.text, 99, 3, 0, 0, 0x41, 0x0D]))!;
    pc.server.handle(Uint8List.fromList(f)..[10] ^= 1);
    await pc.settle();
    expect(pc.text.toString(), isEmpty);
    pc.server.handle(f);
    await pc.settle();
    expect(pc.text.toString(), 'A\n');
    pc.server.handle(f); // again
    await pc.settle();
    expect(pc.text.toString(), 'A\n');
  });

  test('TEXT on channel 0 reaches the console, CR as newline', () async {
    expect(await pc.send(T.text, [0, ...latin1.encode('HELLO\r')]), [T.ack, 0]);
    expect(await pc.send(T.text, [0, ...latin1.encode('A1B\r')]), [T.ack, 0]);
    expect(pc.text.toString(), 'HELLO\nA1B\n');
    expect(await pc.send(T.text, [0, 0x0C, ...latin1.encode('X\r')]), [T.ack, 0]);
    expect(pc.text.toString(), 'HELLO\nA1B\n\fX\n'); // FF passed on for the console to clear
  });

  test('a save writes the file; an existing one needs overwrite', () async {
    final data = List<int>.generate(150, (i) => i);
    expect(await pc.send(T.filePut, [0, 0, 0, ...u32(sizeUnknown), ...str8('T')]), [T.ack, 0]);
    expect(await pc.send(T.fileData, data.sublist(0, 60)), [T.ack, 0]);
    expect(await pc.send(T.fileData, data.sublist(60)), [T.ack, 0]);
    expect(await pc.send(T.fileEnd, []), [T.ack, 0]);
    expect(File('${dir.path}/T').readAsBytesSync(), data);

    expect(await pc.send(T.filePut, [0, 0, 0, ...u32(3), ...str8('T')]), [T.err, E.exists]);
    expect(await pc.send(T.filePut, [0, 0, 1, ...u32(3), ...str8('T')]), [T.ack, 0]);
    expect(await pc.send(T.fileData, [7, 8]), [T.ack, 0]);
    expect(await pc.send(T.fileEnd, []), [T.err, E.io]); // 2 bytes, 3 promised
    expect(File('${dir.path}/T').readAsBytesSync(), data); // left as it was

    expect(await pc.send(T.filePut, [0, 0, 0, ...u32(9), ...str8('A/B')]), [T.err, E.io]);
    expect(await pc.send(T.filePut, [1, 0, 0, ...u32(9), ...str8('X')]), [T.err, E.unsupported]);
  });

  // [pen][x i16][y i32], little-endian
  List<int> prefix(int pen, int x, int y) =>
      [pen, x & 0xFF, x >> 8 & 0xFF, y & 0xFF, y >> 8 & 0xFF, y >> 16 & 0xFF, y >> 24 & 0xFF];

  test('PLOT draws on the paper: absolute and relative ops, pen changes', () async {
    // pen 1 at (100, -40); DRAW to (300, -40); pen 3; DRAW_REL (-5, +7);
    // MOVE_REL (1, 1); DRAW (0, -2000) far down the paper
    final p = [
      ...prefix(1, 100, -40),
      PlotOp.draw, 44, 1, ...u32(-40 & 0xFFFFFFFF), //
      PlotOp.pen, 3,
      PlotOp.drawRel, 0xFB, 7,
      PlotOp.moveRel, 1, 1,
      PlotOp.draw, 0, 0, ...u32(-2000 & 0xFFFFFFFF),
    ];
    expect(await pc.send(T.plot, p), [T.ack, 0]);
    await pc.settle();
    expect(pc.paper.lines.map((l) => l.toString()).toList(), [
      'pen 1 (100,-40)-(300,-40)',
      'pen 3 (300,-40)-(295,-33)',
      'pen 3 (296,-32)-(0,-2000)',
    ]);
    expect([pc.paper.pen, pc.paper.x, pc.paper.y], [3, 0, -2000]);
    expect([pc.paper.top, pc.paper.bottom], [-32, -2000]);
    // a frame with no operations just says where the pen rests
    expect(await pc.send(T.plot, prefix(0, 860, -2100)), [T.ack, 0]);
    await pc.settle();
    expect(pc.paper.lines.length, 3);
    expect([pc.paper.x, pc.paper.y, pc.paper.bottom], [860, -2100, -2100]);
    // malformed: an unknown op, or one cut short
    expect(await pc.send(T.plot, [...prefix(0, 0, 0), 9]), [T.err, E.badFrame]);
    expect(await pc.send(T.plot, [...prefix(0, 0, 0), PlotOp.draw, 1]), [T.err, E.badFrame]);
    expect(await pc.send(T.plot, [1, 2]), [T.err, E.badFrame]);
  });

  test('without a paper, PLOT is ERR UNSUPPORTED (the PC-1500\'s ERROR 27)', () async {
    final noPrinter = FakePc(dir, plots: false);
    await noPrinter.link();
    expect(await noPrinter.send(T.plot, prefix(0, 0, 0)), [T.err, E.unsupported]);
  });

  test('a load sends FILE_PUT, the data in frames, then FILE_END', () async {
    final data = List<int>.generate(150, (i) => 255 - i);
    File('${dir.path}/P').writeAsBytesSync(data);
    pc.frames.clear(); // the server's HELLO
    expect(await pc.send(T.fileGet, [0, ...str8('P')]), [T.ack, 0]);
    await pc.settle();
    expect(pc.frames.first[0], T.filePut);
    expect(pc.frames.last[0], T.fileEnd);
    final got = <int>[];
    for (final f in pc.frames.where((f) => f[0] == T.fileData)) {
      expect(f.length, lessThanOrEqualTo(pc.server.frameMax - sealOverhead)); // as opened: sealed, it fits
      got.addAll(f.sublist(4));
    }
    expect(got, data);
    expect(await pc.send(T.fileGet, [0, ...str8('NONE')]), [T.err, E.notFound]);
  });
}
