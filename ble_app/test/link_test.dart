import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:pc1500_ble/link.dart';
import 'package:pc1500_ble/main.dart';
import 'package:pc1500_ble/plot.dart';

/// Plays the PC-1500 against a LinkServer: sends frames, and ACKs every
/// frame the server sends that needs an answer.
class FakePc {
  FakePc(Directory dir, {bool plots = true}) {
    server = LinkServer(
        send: _fromServer, filesDir: dir, onText: text.write, onLog: (_) {}, onPlot: plots ? paper.add : null);
    server.frameMax = 64;
  }

  late final LinkServer server;
  final paper = PlotPaper();
  final text = StringBuffer();
  final answers = <List<int>>[]; // [type, seq, code] of ACK/ERR the server sent
  final frames = <Uint8List>[]; // everything else the server sent
  int _seq = 0;

  Future<void> _fromServer(Uint8List f) async {
    if (f[0] == T.ack || f[0] == T.err) {
      answers.add([f[0], f[1], f.length > 4 ? f[4] : 0]);
    } else {
      frames.add(f);
      server.handle(Uint8List.fromList([T.ack, f[1], 0, 0]));
    }
  }

  /// Sends a frame; returns the server's answer as [type, code].
  Future<List<int>> send(int type, List<int> payload) async {
    final seq = _seq++ & 0xFF;
    final before = answers.length;
    server.handle(Uint8List.fromList([type, seq, payload.length & 0xFF, payload.length >> 8, ...payload]));
    for (var i = 0; i < 100 && answers.length == before; i++) {
      await Future<void>.delayed(const Duration(milliseconds: 1));
    }
    final a = answers.length > before ? answers[before] : [0, 0, -1];
    expect(a[1], seq);
    return [a[0], a[2]];
  }

  Future<void> settle() => Future<void>.delayed(const Duration(milliseconds: 20));
}

List<int> str8(String s) => [s.length, ...latin1.encode(s)];
List<int> u32(int v) => [v & 0xFF, v >> 8 & 0xFF, v >> 16 & 0xFF, v >> 24 & 0xFF];

void main() {
  late Directory dir;
  late FakePc pc;
  setUp(() {
    dir = Directory.systemTemp.createTempSync('link_test');
    pc = FakePc(dir);
  });
  tearDown(() => dir.deleteSync(recursive: true));

  test('Link UUIDs share one base', () {
    expect(linkRxUuid.substring(8), linkServiceUuid.substring(8));
    expect(linkTxUuid.substring(8), linkServiceUuid.substring(8));
  });

  test('HELLO is answered with the server\'s own HELLO', () async {
    expect(await pc.send(T.hello, [1, 1, ...str8('PC-1500')]), [T.ack, 0]);
    await pc.settle();
    expect(pc.server.peerName, 'PC-1500');
    expect(pc.frames.single[0], T.hello);
    expect(latin1.decode(pc.frames.single.sublist(7)), 'PC1500-SRV');
    expect(await pc.send(T.hello, [9, 1, 0]), [T.err, E.unsupported]);
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
    expect(await noPrinter.send(T.plot, prefix(0, 0, 0)), [T.err, E.unsupported]);
  });

  test('a load sends FILE_PUT, the data in frames, then FILE_END', () async {
    final data = List<int>.generate(150, (i) => 255 - i);
    File('${dir.path}/P').writeAsBytesSync(data);
    expect(await pc.send(T.fileGet, [0, ...str8('P')]), [T.ack, 0]);
    await pc.settle();
    expect(pc.frames.first[0], T.filePut);
    expect(pc.frames.last[0], T.fileEnd);
    final got = <int>[];
    for (final f in pc.frames.where((f) => f[0] == T.fileData)) {
      expect(f.length, lessThanOrEqualTo(pc.server.frameMax));
      got.addAll(f.sublist(4));
    }
    expect(got, data);
    expect(await pc.send(T.fileGet, [0, ...str8('NONE')]), [T.err, E.notFound]);
  });
}
