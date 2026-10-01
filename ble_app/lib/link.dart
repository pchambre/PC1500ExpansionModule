// The PC-1500 Link protocol, server side: RP2350/BLE_PROTOCOL.md. Plain
// Dart with no Bluetooth in it, so it can be tested on its own: frames come
// in through handle() and go out through `send`.
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

/// Frame types (BLE_PROTOCOL.md sec.5).
class T {
  static const hello = 0x01, bye = 0x02, text = 0x10;
  static const filePut = 0x20, fileData = 0x21, fileEnd = 0x22, fileGet = 0x23, fileAbort = 0x24;
  static const plot = 0x40; // the CE-150 stand-in's drawing (2026-09-30)
  static const ack = 0x7E, err = 0x7F;
}

/// ERR codes.
class E {
  static const badFrame = 1, unsupported = 2, notFound = 3, exists = 4, io = 5, busy = 6, aborted = 7;
}

const protocolVersion = 1;
const kindServer = 2;
const targetServer = 0;
const kindUnknown = 0xFF;
const sizeUnknown = 0xFFFFFFFF;
const answerTimeout = Duration(seconds: 5);

class LinkServer {
  LinkServer({
    required this.send,
    required this.filesDir,
    required this.onText,
    required this.onLog,
    this.onPlot,
    this.name = 'PC1500-SRV',
  });

  /// A PLOT payload (plot.dart's PlotPaper.add); false if malformed. Without
  /// one, PLOT is ERR UNSUPPORTED, which the PC-1500 reports as ERROR 27.
  final bool Function(Uint8List payload)? onPlot;

  /// One frame out (a TX notification).
  final Future<void> Function(Uint8List frame) send;
  final Directory filesDir;

  /// TEXT on the console channel, with CR turned into a newline; a form
  /// feed (BLCLS) is passed on, and means "clear the console".
  final void Function(String text) onText;
  final void Function(String line) onLog;
  final String name;

  /// The largest frame, MTU - 3 (sec.2); set when the MTU is known.
  int frameMax = 20;
  String? peerName;

  int _txSeq = 0;
  int? _pendingSeq;
  Completer<int>? _pending; // 0 for ACK, else the ERR code
  Future<void> _queue = Future.value();

  // A save in progress.
  String? _putName;
  int _putSize = 0;
  BytesBuilder? _putData;

  /// Starts afresh for a new connection.
  void reset() {
    _txSeq = 0;
    _pending = null;
    _putName = null;
    _putData = null;
    peerName = null;
  }

  /// One frame in. Answers (ACK/ERR) are taken at once -- a transfer this
  /// side is sending waits on them -- and everything else in order.
  void handle(Uint8List frame) {
    if (frame.length < 4 || frame.length - 4 != (frame[2] | frame[3] << 8)) {
      onLog('Bad frame (${frame.length} bytes)');
      return;
    }
    final type = frame[0], seq = frame[1];
    if (type == T.ack || type == T.err) {
      if (_pending != null && seq == _pendingSeq && !_pending!.isCompleted) {
        _pending!.complete(type == T.ack ? 0 : (frame.length > 4 ? frame[4] : E.badFrame));
      }
      return;
    }
    final payload = Uint8List.sublistView(frame, 4);
    _queue = _queue.then((_) => _process(type, seq, payload)).catchError((Object e) => onLog('Error: $e'));
  }

  Future<void> _process(int type, int seq, Uint8List p) async {
    switch (type) {
      case T.hello:
        if (p.length < 3 || p[0] != protocolVersion || 3 + p[2] > p.length) return _answer(seq, E.unsupported);
        peerName = latin1.decode(p.sublist(3, 3 + p[2]));
        onLog('HELLO from $peerName');
        await _answer(seq);
        final nameBytes = latin1.encode(name);
        final r = await _request(T.hello, [protocolVersion, kindServer, nameBytes.length, ...nameBytes]);
        if (r != 0) onLog('Our HELLO was refused ($r)');
      case T.bye:
        onLog('BYE');
        await _answer(seq);
      case T.text:
        if (p.isEmpty) return _answer(seq, E.badFrame);
        if (p[0] == 0) onText(latin1.decode(p.sublist(1), allowInvalid: true).replaceAll('\r', '\n'));
        await _answer(seq);
      case T.filePut:
        await _filePut(seq, p);
      case T.fileData:
        if (_putData == null) return _answer(seq, E.badFrame);
        _putData!.add(p);
        await _answer(seq);
      case T.fileEnd:
        await _fileEnd(seq);
      case T.fileAbort:
        if (_putName != null) onLog('Save of $_putName abandoned');
        _putName = null;
        _putData = null;
        await _answer(seq);
      case T.fileGet:
        await _fileGet(seq, p);
      case T.plot:
        if (onPlot == null) return _answer(seq, E.unsupported);
        await _answer(seq, onPlot!(Uint8List.fromList(p)) ? 0 : E.badFrame);
      default:
        await _answer(seq, E.unsupported);
    }
  }

  // target, kind, flags, size (u32 LE), name (str8)
  Future<void> _filePut(int seq, Uint8List p) async {
    if (p.length < 8 || 8 + p[7] > p.length) return _answer(seq, E.badFrame);
    if (p[0] != targetServer) return _answer(seq, E.unsupported);
    if (_putData != null) return _answer(seq, E.busy);
    final name = _fileName(p.sublist(8, 8 + p[7]));
    if (name == null) return _answer(seq, E.io);
    final overwrite = p[2] & 1 != 0;
    if (!overwrite && File(_path(name)).existsSync()) return _answer(seq, E.exists);
    _putName = name;
    _putSize = ByteData.sublistView(p, 3, 7).getUint32(0, Endian.little);
    _putData = BytesBuilder();
    onLog('Saving $name');
    await _answer(seq);
  }

  Future<void> _fileEnd(int seq) async {
    final data = _putData, name = _putName;
    _putData = null;
    _putName = null;
    if (data == null || name == null) return _answer(seq, E.badFrame);
    final bytes = data.takeBytes();
    if (_putSize != sizeUnknown && bytes.length != _putSize) {
      onLog('$name: got ${bytes.length} bytes, expected $_putSize');
      return _answer(seq, E.io);
    }
    try {
      filesDir.createSync(recursive: true);
      File(_path(name)).writeAsBytesSync(bytes);
    } catch (e) {
      onLog('$name: $e');
      return _answer(seq, E.io);
    }
    onLog('Saved $name (${bytes.length} bytes)');
    await _answer(seq);
  }

  // target, name (str8)
  Future<void> _fileGet(int seq, Uint8List p) async {
    if (p.length < 2 || 2 + p[1] > p.length) return _answer(seq, E.badFrame);
    if (p[0] != targetServer) return _answer(seq, E.unsupported);
    final name = _fileName(p.sublist(2, 2 + p[1]));
    final file = name == null ? null : File(_path(name));
    if (file == null || !file.existsSync()) return _answer(seq, E.notFound);
    final bytes = file.readAsBytesSync();
    await _answer(seq);
    onLog('Sending $name (${bytes.length} bytes)');
    final nameBytes = latin1.encode(name!);
    final header = BytesBuilder()
      ..add([targetServer, kindUnknown, 0])
      ..add((ByteData(4)..setUint32(0, bytes.length, Endian.little)).buffer.asUint8List())
      ..add([nameBytes.length, ...nameBytes]);
    if (await _request(T.filePut, header.takeBytes()) != 0) return onLog('$name: refused');
    final chunk = frameMax - 4;
    for (var at = 0; at < bytes.length; at += chunk) {
      final end = at + chunk < bytes.length ? at + chunk : bytes.length;
      final r = await _request(T.fileData, bytes.sublist(at, end));
      if (r != 0) return onLog('$name: stopped at byte $at ($r)');
    }
    if (await _request(T.fileEnd, const []) == 0) onLog('Sent $name');
  }

  /// A plain file name for the store: no folders, nothing Windows rejects.
  String? _fileName(List<int> raw) {
    final name = latin1.decode(raw).trim();
    if (name.isEmpty || name == '.' || name == '..' || RegExp(r'[\\/:*?"<>|\x00-\x1F]').hasMatch(name)) return null;
    return name;
  }

  String _path(String name) => '${filesDir.path}${Platform.pathSeparator}$name';

  Future<void> _answer(int seq, [int error = 0]) =>
      send(_frame(error == 0 ? T.ack : T.err, seq, error == 0 ? const [] : [error]));

  /// A frame out and its answer: 0 for ACK, else the ERR code (or
  /// E.aborted if none came -- sec.4).
  Future<int> _request(int type, List<int> payload) async {
    final seq = _txSeq;
    _txSeq = (_txSeq + 1) & 0xFF;
    _pendingSeq = seq;
    _pending = Completer<int>();
    await send(_frame(type, seq, payload));
    return _pending!.future.timeout(answerTimeout, onTimeout: () {
      onLog('No answer from the PC-1500');
      return E.aborted;
    });
  }

  static Uint8List _frame(int type, int seq, List<int> payload) =>
      Uint8List.fromList([type, seq, payload.length & 0xFF, payload.length >> 8, ...payload]);
}
