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
  FakePc(Directory dir,
      {bool plots = true, PairingStore? store, Future<int> Function(int op)? onPaper, void Function(String)? onDebug}) {
    serverStore = store ?? MemoryPairingStore();
    server = LinkServer(
        send: _fromServer,
        filesDir: dir,
        onText: text.write,
        onLog: (_) {},
        onPlot: plots ? paper.add : null,
        onPaper: onPaper,
        onDebug: onDebug,
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

  test('TIME is ACKed with this side\'s clock: Unix ms, then the UTC offset in minutes', () async {
    final before = DateTime.now().millisecondsSinceEpoch;
    expect(await pc.send(T.time, const []), [T.ack, 0]);
    final p = ByteData.sublistView(Uint8List.fromList(pc.lastPayload));
    expect(p.lengthInBytes, 10);
    final ms = p.getUint32(0, Endian.little) + p.getUint32(4, Endian.little) * 0x100000000;
    expect(ms, inInclusiveRange(before, DateTime.now().millisecondsSinceEpoch));
    expect(p.getInt16(8, Endian.little), DateTime.now().timeZoneOffset.inMinutes);
  });

  test('timePayload: a time past 2^32 ms', () {
    final t = DateTime.fromMillisecondsSinceEpoch(0x123456789AB);
    final p = ByteData.sublistView(timePayload(t));
    expect(p.getUint32(0, Endian.little) + p.getUint32(4, Endian.little) * 0x100000000, 0x123456789AB);
    expect(p.getInt16(8, Endian.little), t.timeZoneOffset.inMinutes);
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
  // DIR: start, path -> total, count, then [flags][size u32][name str8]...
  Future<List<List<Object>>> listing(FakePc pc, [String path = '']) async {
    final all = <List<Object>>[];
    var total = -1;
    while (total < 0 || all.length < total) {
      expect(await pc.send(T.dir, [all.length & 0xFF, all.length >> 8, ...str8(path)]), [T.ack, 0]);
      final p = pc.lastPayload;
      total = p[0] | p[1] << 8;
      if (total > all.length) expect(p[2], greaterThan(0));
      var at = 3;
      for (var i = 0; i < p[2]; i++) {
        final size = ByteData.sublistView(p, at + 1, at + 5).getUint32(0, Endian.little);
        all.add([p[at] & 1 == 1 ? 'D' : 'F', size, latin1.decode(p.sublist(at + 6, at + 6 + p[at + 5]))]);
        at += 6 + p[at + 5];
      }
      expect(at, p.length);
    }
    return all;
  }

  Future<List<int>> fs(FakePc pc, int op, [String a = '', String b = '', int flags = 0]) =>
      pc.send(T.fs, [op, flags, ...str8(a), ...str8(b)]);

  Future<String> pwd(FakePc pc) async {
    expect(await fs(pc, Op.pwd), [T.ack, 0]);
    return latin1.decode(pc.lastPayload.sublist(1, 1 + pc.lastPayload[0]));
  }

  test('DIR pages through a directory: folders first, then files, by name', () async {
    Directory('${dir.path}/zeta').createSync();
    Directory('${dir.path}/Alpha').createSync();
    File('${dir.path}/b.bas').writeAsBytesSync(List.filled(12, 0));
    File('${dir.path}/A long file name.txt').writeAsBytesSync([1]);
    File('${dir.path}/C').writeAsBytesSync([]);
    expect(await listing(pc), [
      ['D', 0, 'Alpha'],
      ['D', 0, 'zeta'],
      ['F', 1, 'A long file name.txt'],
      ['F', 12, 'b.bas'],
      ['F', 0, 'C'],
    ]);
    expect(await listing(pc, '/zeta'), isEmpty);
    expect(await pc.send(T.dir, [0, 0, ...str8('NONE')]), [T.err, E.notFound]);
    expect(await pc.send(T.dir, [0, 0, ...str8('..')]), [T.err, E.notFound]); // never above the root
  });

  test('CD, PWD, MKDIR, RMDIR; paths reach FILE_PUT and FILE_GET', () async {
    expect(await pwd(pc), '/');
    expect(await fs(pc, Op.mkdir, 'GAMES'), [T.ack, 0]);
    expect(await fs(pc, Op.mkdir, 'GAMES'), [T.err, E.exists]);
    expect(await fs(pc, Op.mkdir, 'NO/SUCH'), [T.err, E.notFound]);
    expect(await fs(pc, Op.cd, 'GAMES'), [T.ack, 0]);
    expect(await pwd(pc), '/GAMES');
    expect(await fs(pc, Op.mkdir, 'OLD'), [T.ack, 0]);
    expect(Directory('${dir.path}/GAMES/OLD').existsSync(), isTrue);
    expect(await fs(pc, Op.cd, 'OLD/../OLD/.'), [T.ack, 0]);
    expect(await pwd(pc), '/GAMES/OLD');
    expect(await fs(pc, Op.cd, '/'), [T.ack, 0]);
    expect(await fs(pc, Op.cd, 'NONE'), [T.err, E.notFound]);
    expect(await fs(pc, Op.cd, '..'), [T.err, E.notFound]);
    expect(await pwd(pc), '/');

    // a save and a load by path, relative to the current directory
    expect(await fs(pc, Op.cd, 'GAMES'), [T.ack, 0]);
    expect(await pc.send(T.filePut, [0, 0, 0, ...u32(2), ...str8('OLD/Pong.bas')]), [T.ack, 0]);
    expect(await pc.send(T.fileData, [5, 6]), [T.ack, 0]);
    expect(await pc.send(T.fileEnd, []), [T.ack, 0]);
    expect(File('${dir.path}/GAMES/OLD/Pong.bas').readAsBytesSync(), [5, 6]);
    pc.frames.clear();
    expect(await pc.send(T.fileGet, [0, ...str8('/GAMES/OLD/Pong.bas')]), [T.ack, 0]);
    await pc.settle();
    expect(pc.frames.first[0], T.filePut);
    expect(await pc.send(T.filePut, [0, 0, 0, ...u32(2), ...str8('OLD')]), [T.err, E.io]); // a directory

    expect(await fs(pc, Op.rmdir, 'OLD'), [T.err, E.io]); // not empty
    expect(await fs(pc, Op.rm, 'OLD'), [T.err, E.notFound]); // RM takes files only
    expect(await fs(pc, Op.rm, 'OLD/Pong.bas'), [T.ack, 0]);
    expect(await fs(pc, Op.rmdir, 'OLD'), [T.ack, 0]);
    expect(Directory('${dir.path}/GAMES/OLD').existsSync(), isFalse);
    expect(await fs(pc, 99), [T.err, E.unsupported]);
  });

  test('the current directory is kept for the next link, and dropped if it goes', () async {
    expect(await fs(pc, Op.mkdir, 'KEEP'), [T.ack, 0]);
    expect(await fs(pc, Op.cd, 'KEEP'), [T.ack, 0]);
    expect(await pc.hello(reconnect: true), isTrue);
    expect(await pwd(pc), '/KEEP');
    Directory('${dir.path}/KEEP').deleteSync();
    expect(await pwd(pc), '/');
  });

  test('CP and MV: into a directory, EXISTS unless overwriting', () async {
    File('${dir.path}/A').writeAsBytesSync([1]);
    File('${dir.path}/B').writeAsBytesSync([2]);
    Directory('${dir.path}/D').createSync();
    expect(await fs(pc, Op.cp, 'A', 'B'), [T.err, E.exists]);
    expect(await fs(pc, Op.cp, 'A', 'B', 1), [T.ack, 0]);
    expect(File('${dir.path}/B').readAsBytesSync(), [1]);
    expect(await fs(pc, Op.cp, 'A', 'D'), [T.ack, 0]); // into D, as A
    expect(File('${dir.path}/D/A').readAsBytesSync(), [1]);
    expect(await fs(pc, Op.cp, 'D', 'E'), [T.err, E.notFound]); // CP takes files only
    expect(await fs(pc, Op.cp, 'NONE', 'E'), [T.err, E.notFound]);
    expect(await fs(pc, Op.mv, 'B', 'C'), [T.ack, 0]);
    expect(File('${dir.path}/B').existsSync(), isFalse);
    expect(await fs(pc, Op.mv, 'A', 'D'), [T.err, E.exists]); // D/A is there
    expect(await fs(pc, Op.mv, 'A', 'D', 1), [T.ack, 0]);
    expect(await fs(pc, Op.mv, 'D', 'E'), [T.ack, 0]); // a directory renamed
    expect(File('${dir.path}/E/A').existsSync(), isTrue);
    expect(await fs(pc, Op.mv, 'C', 'bad:name'), [T.err, E.io]);
  });

  test('PAPER: each op to the app, its answer back; without a printer, UNSUPPORTED', () async {
    expect(await pc.send(T.paper, [1]), [T.err, E.unsupported]);
    final ops = <int>[];
    final printer = FakePc(dir, onPaper: (op) async {
      ops.add(op);
      return op == 2 ? E.io : 0;
    });
    await printer.link();
    expect(await printer.send(T.paper, [1]), [T.ack, 0]);
    expect(await printer.send(T.paper, [2]), [T.err, E.io]);
    expect(await printer.send(T.paper, [3]), [T.ack, 0]);
    expect(await printer.send(T.paper, []), [T.err, E.badFrame]);
    expect(ops, [1, 2, 3]);
  });

  test('DEBUG: the empty probe is ACKed, text reaches the pane; without one, UNSUPPORTED', () async {
    expect(await pc.send(T.debug, []), [T.err, E.unsupported]);
    final got = StringBuffer();
    final debug = FakePc(dir, onDebug: got.write);
    await debug.link();
    expect(await debug.send(T.debug, []), [T.ack, 0]);
    expect(got.toString(), '');
    expect(await debug.send(T.debug, latin1.encode('[ble] up\r\nMLOG W: x\n')), [T.ack, 0]);
    expect(got.toString(), '[ble] up\nMLOG W: x\n');
  });

  test('a PC-1500 that isn\'t paired gets nothing from the folders', () async {
    final other = FakePc(dir);
    expect(await other.hello(auth: false), isFalse);
    expect(await other.send(T.dir, [0, 0, 0]), [T.err, E.notPaired]);
    expect(await other.send(T.fs, [Op.pwd, 0, 0, 0]), [T.err, E.notPaired]);
  });
}
