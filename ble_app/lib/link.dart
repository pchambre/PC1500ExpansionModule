// The PC-1500 Link protocol, server side: RP2350/BLE_PROTOCOL.md. Plain
// Dart with no Bluetooth in it, so it can be tested on its own: frames come
// in through handle() and go out through `send`.
//
// Every link is authenticated (sec.7, 2026-10-03): a PC-1500 pairs once
// with BLPAIR, this side's user checking the code it shows; after that its
// HELLOs prove it, and every frame is sealed. Until then only HELLO, BYE,
// AUTH and the pairing frames are answered -- files and the console are
// out of reach of anything not paired.
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'secure.dart';

/// Frame types (BLE_PROTOCOL.md sec.5).
class T {
  static const hello = 0x01, bye = 0x02, text = 0x10;
  static const auth = 0x03, pairStart = 0x04, pairNonce = 0x05, pairConfirm = 0x06; // sec.7
  static const filePut = 0x20, fileData = 0x21, fileEnd = 0x22, fileGet = 0x23, fileAbort = 0x24;
  static const dir = 0x27, fs = 0x28; // the shared folder's tree (2026-11-13)
  static const plot = 0x40; // the CE-150 stand-in's drawing (2026-09-30)
  static const time = 0x41; // our clock, for the PC-1500's (2026-10-08)
  static const paper = 0x42; // print the console or the paper, clear the paper (2026-11-14)
  static const display = 0x50; // the PC-1500's screen text (2026-10-17, screen.dart)
  static const debug = 0x51; // the PC-1500 module's debug text (2026-11-14)
  static const ack = 0x7E, err = 0x7F;
}

/// ERR codes.
class E {
  static const badFrame = 1, unsupported = 2, notFound = 3, exists = 4, io = 5, busy = 6, aborted = 7;
  static const notPaired = 8, authFailed = 9;
}

const protocolVersion = 2; // 2: sec.7's security
const kindServer = 2;
const targetServer = 0;
const kindUnknown = 0xFF;
const kindBasic = 0, kindM = 1;
const sizeUnknown = 0xFFFFFFFF;

/// A file's kind for FILE_GET's FILE_PUT (2026-11-16, BLE_PROTOCOL.md
/// "Files"), so BLLOAD without M loads it as what it is: BASIC when all of
/// it is BASIC's line layout -- [number hi][lo][length][length bytes, the
/// last CR], numbers above 0 and rising, then FF as its last byte -- or
/// it's empty; anything else is M (its [load][call] header, or a banked
/// one's FF 'B', breaks the layout). As keywords.c's file_is_basic().
int fileKind(List<int> b) {
  var i = 0, prev = 0;
  while (i < b.length) {
    if (b[i] == 0xFF) return i == b.length - 1 ? kindBasic : kindM;
    if (i + 3 > b.length) return kindM;
    final num = b[i] << 8 | b[i + 1], len = b[i + 2];
    if (num == 0 || num <= prev || len == 0) return kindM;
    i += 3 + len;
    if (i > b.length || b[i - 1] != 0x0D) return kindM;
    prev = num;
  }
  return b.isEmpty ? kindBasic : kindM;
}
const answerTimeout = Duration(seconds: 5);

/// FS operations (BLE_PROTOCOL.md "Host folders").
class Op {
  static const cd = 1, pwd = 2, mkdir = 3, rmdir = 4, rm = 5, cp = 6, mv = 7;
}

/// A path that isn't one: `..` above the root, or a name this host can't
/// store.
class BadPath implements Exception {}

/// TIME's answer (BLE_PROTOCOL.md sec.5): `now` as Unix ms (i64), then its
/// offset from UTC in minutes (i16), little-endian -- the PC-1500's clock
/// is set to this side's local time.
Uint8List timePayload(DateTime now) {
  final ms = now.millisecondsSinceEpoch;
  return (ByteData(10)
        ..setUint32(0, ms % 0x100000000, Endian.little)
        ..setUint32(4, ms ~/ 0x100000000, Endian.little)
        ..setInt16(8, now.timeZoneOffset.inMinutes, Endian.little))
      .buffer
      .asUint8List();
}

class LinkServer {
  LinkServer({
    required this.send,
    required this.filesDir,
    required this.onText,
    required this.onLog,
    required this.pairings,
    this.onPlot,
    this.onDisplay,
    this.onPaper,
    this.onDebug,
    this.onPairRequest,
    this.name = 'PC1500-SRV',
  });

  /// This side's identity and the PC-1500s paired with it.
  final PairingStore pairings;

  /// A PC-1500's BLPAIR waiting for this side's user: the code to compare
  /// with the one it shows, then answerPairing(). Called again with null
  /// once the question is moot (answered, or the link gone).
  final void Function(String? code, String? peer)? onPairRequest;

  /// A PLOT payload (plot.dart's PlotPaper.add); false if malformed. Without
  /// one, PLOT is ERR UNSUPPORTED, which the PC-1500 reports as ERROR 27.
  final bool Function(Uint8List payload)? onPlot;

  /// A DISPLAY payload (screen.dart's ScreenModel.add); false if malformed.
  /// Without one, DISPLAY is ERR UNSUPPORTED, and the PC-1500 stops sending.
  final bool Function(Uint8List payload)? onDisplay;

  /// PAPER's op (BLE_PROTOCOL.md "Paper": 1 print the console, 2 print the
  /// plotter's paper, 3 clear it): 0 for ACK, else the ERR code. Without
  /// one, PAPER is ERR UNSUPPORTED.
  final Future<int> Function(int op)? onPaper;

  /// DEBUG's text (MCONF BLDEBUG), LF line ends. Without one, DEBUG is ERR
  /// UNSUPPORTED and the PC-1500 sends no more of it on this link.
  final void Function(String text)? onDebug;

  /// One frame out (a TX notification).
  final Future<void> Function(Uint8List frame) send;

  /// Where BLSAVE/BLLOAD files live: the root of the folders a PC-1500
  /// sees. Settings can change it between transfers.
  Directory filesDir;

  /// Each paired PC-1500's current directory (by its id, in hex), as names
  /// below filesDir. Kept while the app runs, so a new link carries on.
  final Map<String, List<String>> _cwds = {};

  /// TEXT on the console channel, with CR turned into a newline; a form
  /// feed (BLCLS) is passed on, and means "clear the console".
  final void Function(String text) onText;
  final void Function(String line) onLog;

  /// This side's name in its HELLO, which the PC-1500 shows on connecting
  /// (settings.dart; the next HELLO takes a change).
  String name;

  /// The largest frame, MTU - 3 (sec.2); set when the MTU is known.
  int frameMax = 20;
  String? peerName;

  /// The link has authenticated: everything is sealed and allowed.
  bool get authenticated => _session != null;

  // sec.7: the HELLO's values, the session, a pairing under way
  Session? _session;
  Uint8List? _peerId, _nonceC, _nonceS, _ltk;
  int _pairStep = 0; // 1: keys exchanged, 2: nonces too, waiting on the user
  bool? _pairAnswer;
  PairKeys? _pairKeys;
  Uint8List? _pkC, _nS, _pairLtk;
  String? _pairName;

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
    _resetSecurity();
  }

  void _resetSecurity() {
    _session = null;
    _peerId = _nonceC = _nonceS = _ltk = null;
    _endPairing();
  }

  void _endPairing() {
    final asked = _pairStep == 2 && _pairAnswer == null;
    _pairStep = 0;
    _pairAnswer = null;
    _pairKeys = null;
    _pkC = _nS = _pairLtk = null;
    if (asked) onPairRequest?.call(null, null);
  }

  /// This side's user's answer to a PC-1500's pairing (onPairRequest).
  void answerPairing(bool accept) {
    if (_pairStep != 2 || _pairAnswer != null) return;
    _pairAnswer = accept;
    onPairRequest?.call(null, null);
    onLog(accept ? 'Pairing accepted; waiting for $_pairName' : 'Pairing refused');
  }

  /// One frame in. Answers (ACK/ERR) are taken at once -- a transfer this
  /// side is sending waits on them -- and everything else in order.
  void handle(Uint8List frame) {
    if (frame.length < 4 || frame.length - 4 != (frame[2] | frame[3] << 8)) {
      onLog('Bad frame (${frame.length} bytes)');
      return;
    }
    if (_session != null) {
      final plain = _session!.open(frame);
      if (plain == null) return onLog('Dropped a frame that failed authentication');
      frame = plain;
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
    const open = {T.hello, T.bye, T.auth, T.pairStart, T.pairNonce, T.pairConfirm};
    if (_session == null && !open.contains(type)) return _answer(seq, E.notPaired);
    if (type == T.filePut || type == T.fileGet || type == T.dir || type == T.fs) {
      try {
        filesDir.createSync(recursive: true); // the root, before anything in it
      } catch (_) {}
    }
    switch (type) {
      case T.hello:
        await _hello(seq, p);
      case T.auth:
        await _auth(seq, p);
      case T.pairStart:
      case T.pairNonce:
      case T.pairConfirm:
        await _pair(type, seq, p);
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
      case T.dir:
        await _dir(seq, p);
      case T.fs:
        await _fs(seq, p);
      case T.plot:
        if (onPlot == null) return _answer(seq, E.unsupported);
        await _answer(seq, onPlot!(Uint8List.fromList(p)) ? 0 : E.badFrame);
      case T.display:
        if (onDisplay == null) return _answer(seq, E.unsupported);
        await _answer(seq, onDisplay!(Uint8List.fromList(p)) ? 0 : E.badFrame);
      case T.paper:
        if (onPaper == null) return _answer(seq, E.unsupported);
        if (p.length != 1) return _answer(seq, E.badFrame);
        await _answer(seq, await onPaper!(p[0]));
      case T.debug:
        if (onDebug == null) return _answer(seq, E.unsupported);
        if (p.isNotEmpty) onDebug!(latin1.decode(p, allowInvalid: true).replaceAll('\r', ''));
        await _answer(seq);
      case T.time:
        onLog("$peerName's clock set");
        await _answer(seq, 0, timePayload(DateTime.now()));
      default:
        await _answer(seq, E.unsupported);
    }
  }

  // ---- sec.7 ----

  // version, kind, name (str8), id 8, nonce 16. Ours adds `known` and, if
  // known, our proof.
  Future<void> _hello(int seq, Uint8List p) async {
    final n = p.length >= 3 ? p[2] : 0;
    if (p.length < 3 + n + idLen + nonceLen || p[0] != protocolVersion) {
      onLog('A PC-1500 with older firmware was refused: update it for pairing');
      return _answer(seq, E.unsupported);
    }
    _resetSecurity();
    peerName = latin1.decode(p.sublist(3, 3 + n));
    _peerId = Uint8List.fromList(p.sublist(3 + n, 3 + n + idLen));
    _nonceC = Uint8List.fromList(p.sublist(3 + n + idLen, 3 + n + idLen + nonceLen));
    _nonceS = randomBytes(nonceLen);
    final pairing = pairings.find(_peerId!);
    _ltk = pairing?.ltk;
    onLog('HELLO from $peerName${pairing == null ? " (not paired)" : ""}');
    await _answer(seq);
    final nameBytes = latin1.encode(name);
    final r = await _request(T.hello, [
      protocolVersion, kindServer, nameBytes.length, ...nameBytes, //
      ...pairings.id, ..._nonceS!, pairing == null ? 0 : 1,
      if (pairing != null) ...authProof(pairing.ltk, 'S', _nonceC!, _nonceS!, _peerId!, pairings.id),
    ]);
    if (r != 0) onLog('Our HELLO was refused ($r)');
  }

  // The PC-1500's proof. The session starts before the ACK -- the last
  // frame in the clear -- goes out, so nothing it seals next is missed.
  Future<void> _auth(int seq, Uint8List p) async {
    final ltk = _ltk;
    if (ltk == null) return _answer(seq, E.notPaired);
    if (_session != null || p.length != proofLen) return _answer(seq, E.badFrame);
    if (!equal16(p, authProof(ltk, 'C', _nonceC!, _nonceS!, _peerId!, pairings.id))) {
      onLog('$peerName failed to prove itself');
      return _answer(seq, E.authFailed);
    }
    _session = Session.start(ltk, _nonceC!, _nonceS!, connector: false);
    _ltk = null;
    onLog('$peerName authenticated');
    await send(_frame(T.ack, seq, const []));
  }

  // PAIR_START [pk_c] -> [pk_s][commit]; PAIR_NONCE [n_c] -> [n_s], and the
  // code for the user; PAIR_CONFIRM [ok][mac_c] -> BUSY until the user
  // answers, then [0] (refused) or [1][mac_s] (paired).
  Future<void> _pair(int type, int seq, Uint8List p) async {
    if (_session != null) return _answer(seq, E.badFrame);
    switch (type) {
      case T.pairStart when p.length == pubLen:
        _endPairing();
        _pairKeys = await PairKeys.fromRandom(randomBytes(32));
        _pkC = Uint8List.fromList(p);
        _nS = randomBytes(nonceLen);
        _pairStep = 1;
        return _answer(seq, 0, [..._pairKeys!.publicKey, ...pairCommit(_pairKeys!.publicKey, _pkC!, _nS!)]);
      case T.pairNonce when p.length == nonceLen && _pairStep == 1:
        final pkS = _pairKeys!.publicKey;
        _pairLtk = _pairKeys!.ltk(_pkC!, _pkC!, pkS, p, _nS!);
        _pairKeys = null;
        if (_pairLtk == null) {
          _endPairing();
          return _answer(seq, E.authFailed);
        }
        _pairStep = 2;
        _pairName = peerName ?? 'PC-1500';
        final code = codeText(pairCode(_pkC!, pkS, p, _nS!));
        onLog('$_pairName wants to pair: code $code');
        await _answer(seq, 0, _nS!);
        onPairRequest?.call(code, _pairName);
      case T.pairConfirm when p.length == 1 + proofLen && _pairStep == 2:
        if (_pairAnswer == null) return _answer(seq, E.busy);
        if (_pairAnswer == false || p[0] != 1) {
          _endPairing();
          return _answer(seq, 0, const [0]);
        }
        final ltk = _pairLtk!;
        if (!equal16(Uint8List.sublistView(p, 1), pairConfirm(ltk, 'C', _peerId!, pairings.id))) {
          _endPairing();
          onLog('Pairing failed: the codes were not the same');
          return _answer(seq, E.authFailed);
        }
        await pairings.add(Pairing(Uint8List.fromList(_peerId!), _pairName!, ltk));
        onLog('Paired with $_pairName');
        final mac = pairConfirm(ltk, 'S', _peerId!, pairings.id);
        _endPairing();
        await _answer(seq, 0, [1, ...mac]);
      default:
        _endPairing();
        await _answer(seq, E.badFrame);
    }
  }

  // target, kind, flags, size (u32 LE), name (str8)
  Future<void> _filePut(int seq, Uint8List p) async {
    if (p.length < 8 || 8 + p[7] > p.length) return _answer(seq, E.badFrame);
    if (p[0] != targetServer) return _answer(seq, E.unsupported);
    if (_putData != null) return _answer(seq, E.busy);
    final name = _fileName(p.sublist(8, 8 + p[7]));
    if (name == null || FileSystemEntity.isDirectorySync(name) || !File(name).parent.existsSync()) {
      return _answer(seq, E.io);
    }
    final overwrite = p[2] & 1 != 0;
    if (!overwrite && File(name).existsSync()) return _answer(seq, E.exists);
    _putName = name;
    _putSize = ByteData.sublistView(p, 3, 7).getUint32(0, Endian.little);
    _putData = BytesBuilder();
    onLog('Saving ${_shown(name)}');
    await _answer(seq);
  }

  Future<void> _fileEnd(int seq) async {
    final data = _putData, name = _putName;
    _putData = null;
    _putName = null;
    if (data == null || name == null) return _answer(seq, E.badFrame);
    final bytes = data.takeBytes();
    if (_putSize != sizeUnknown && bytes.length != _putSize) {
      onLog('${_shown(name)}: got ${bytes.length} bytes, expected $_putSize');
      return _answer(seq, E.io);
    }
    try {
      File(name).writeAsBytesSync(bytes);
    } catch (e) {
      onLog('${_shown(name)}: $e');
      return _answer(seq, E.io);
    }
    onLog('Saved ${_shown(name)} (${bytes.length} bytes)');
    await _answer(seq);
  }

  // target, name (str8)
  Future<void> _fileGet(int seq, Uint8List p) async {
    if (p.length < 2 || 2 + p[1] > p.length) return _answer(seq, E.badFrame);
    if (p[0] != targetServer) return _answer(seq, E.unsupported);
    final path = _fileName(p.sublist(2, 2 + p[1]));
    final file = path == null ? null : File(path);
    if (file == null || !file.existsSync()) return _answer(seq, E.notFound);
    final bytes = file.readAsBytesSync();
    await _answer(seq);
    final name = _shown(path!);
    onLog('Sending $name (${bytes.length} bytes)');
    final nameBytes = latin1.encode(_leaf(file));
    final header = BytesBuilder()
      ..add([targetServer, fileKind(bytes), 0])
      ..add((ByteData(4)..setUint32(0, bytes.length, Endian.little)).buffer.asUint8List())
      ..add([nameBytes.length, ...nameBytes]);
    if (await _request(T.filePut, header.takeBytes()) != 0) return onLog('$name: refused');
    final chunk = frameMax - 4 - sealOverhead;
    for (var at = 0; at < bytes.length; at += chunk) {
      final end = at + chunk < bytes.length ? at + chunk : bytes.length;
      final r = await _request(T.fileData, bytes.sublist(at, end));
      if (r != 0) return onLog('$name: stopped at byte $at ($r)');
    }
    if (await _request(T.fileEnd, const []) == 0) onLog('Sent $name');
  }

  // ---- the shared folder's tree (BLE_PROTOCOL.md "Host folders") ----

  String get _peerKey => (_peerId ?? const <int>[]).map((b) => b.toRadixString(16).padLeft(2, '0')).join();

  /// This PC-1500's current directory, as names below filesDir; back to
  /// the root if it has gone.
  List<String> get _cwd {
    final cwd = _cwds[_peerKey] ?? const <String>[];
    if (cwd.isNotEmpty && !Directory(_hostPath(cwd)).existsSync()) {
      _cwds.remove(_peerKey);
      return const [];
    }
    return cwd;
  }

  /// A PC-1500 path ('/' between names; from the root if it starts with
  /// one, else from the current directory) as names below filesDir.
  /// Throws BadPath for `..` above the root, or a name this host can't
  /// store.
  List<String> _resolve(String path) {
    final names = path.startsWith('/') ? <String>[] : [..._cwd];
    for (final part in path.split('/')) {
      if (part.isEmpty || part == '.') continue;
      if (part == '..') {
        if (names.isEmpty) throw BadPath();
        names.removeLast();
        continue;
      }
      if (RegExp(r'[\\:*?"<>|\x00-\x1F]').hasMatch(part) || part.trim() != part || part.endsWith('.')) {
        throw BadPath();
      }
      names.add(part);
    }
    return names;
  }

  String _hostPath(List<String> names) => [filesDir.path, ...names].join(Platform.pathSeparator);

  /// A file's path on this host, for FILE_PUT/FILE_GET; null if it isn't one.
  String? _fileName(List<int> raw) {
    try {
      final names = _resolve(latin1.decode(raw));
      return names.isEmpty ? null : _hostPath(names);
    } on BadPath {
      return null;
    }
  }

  /// A host path as the PC-1500 writes it, for the log.
  String _shown(String hostPath) {
    final rel = hostPath.length > filesDir.path.length ? hostPath.substring(filesDir.path.length + 1) : '';
    return '/${rel.replaceAll(Platform.pathSeparator, '/')}';
  }

  static String _leaf(FileSystemEntity e) => e.uri.pathSegments.lastWhere((s) => s.isNotEmpty);

  // start (u16 LE), path (str8) -> ACK: total u16, count u8, then entries:
  // flags u8 (bit 0 directory), size u32, name str8 -- as many as fit.
  Future<void> _dir(int seq, Uint8List p) async {
    if (p.length < 3 || 3 + p[2] > p.length) return _answer(seq, E.badFrame);
    final start = p[0] | p[1] << 8;
    final Directory dir;
    try {
      dir = Directory(_hostPath(_resolve(latin1.decode(p.sublist(3, 3 + p[2])))));
    } on BadPath {
      return _answer(seq, E.notFound);
    }
    if (!dir.existsSync()) return _answer(seq, E.notFound);
    final List<FileSystemEntity> all;
    try {
      all = dir.listSync(followLinks: false).where((e) => e is File || e is Directory).toList();
    } catch (e) {
      onLog('Listing: $e');
      return _answer(seq, E.io);
    }
    all.sort((a, b) {
      final ad = a is Directory, bd = b is Directory;
      if (ad != bd) return ad ? -1 : 1;
      return _leaf(a).toLowerCase().compareTo(_leaf(b).toLowerCase());
    });
    await _answer(seq, 0, dirPage(all, start, frameMax - 4 - sealOverhead));
  }

  /// One DIR answer: entries from `start`, as many as fit `room` bytes.
  static Uint8List dirPage(List<FileSystemEntity> all, int start, int room) {
    final out = BytesBuilder()..add([all.length & 0xFF, (all.length >> 8) & 0xFF, 0]);
    var count = 0;
    for (var i = start; i < all.length && count < 255; i++) {
      final e = all[i];
      final leaf = _leaf(e);
      final name = latin1.encode(leaf.length > 255 ? leaf.substring(0, 255) : leaf, );
      if (out.length + 6 + name.length > room) break;
      final size = e is File ? e.lengthSync() : 0;
      out
        ..add([e is Directory ? 1 : 0])
        ..add((ByteData(4)..setUint32(0, size > 0xFFFFFFFF ? 0xFFFFFFFF : size, Endian.little)).buffer.asUint8List())
        ..add([name.length, ...name]);
      count++;
    }
    final page = out.takeBytes();
    page[2] = count;
    return page;
  }

  // op, flags, name (str8), name2 (str8; may be left out)
  Future<void> _fs(int seq, Uint8List p) async {
    if (p.length < 3 || 3 + p[2] > p.length) return _answer(seq, E.badFrame);
    final op = p[0], overwrite = p[1] & 1 != 0;
    final at2 = 3 + p[2];
    if (at2 < p.length && at2 + 1 + p[at2] > p.length) return _answer(seq, E.badFrame);
    final name = latin1.decode(p.sublist(3, at2));
    final name2 = at2 < p.length ? latin1.decode(p.sublist(at2 + 1, at2 + 1 + p[at2])) : '';
    Object r;
    try {
      r = _fsOp(op, name, name2, overwrite);
    } on BadPath {
      r = op == Op.mkdir || op == Op.cp || op == Op.mv ? E.io : E.notFound;
    } catch (e) {
      onLog('$e');
      r = E.io;
    }
    if (r is List<int>) return _answer(seq, 0, r);
    return _answer(seq, r as int);
  }

  /// One FS op: 0 or an ERR code, or PWD's answer.
  Object _fsOp(int op, String name, String name2, bool overwrite) {
    switch (op) {
      case Op.pwd:
        final path = latin1.encode('/${_cwd.join('/')}');
        return [path.length, ...path];
      case Op.cd:
        final names = _resolve(name);
        if (!Directory(_hostPath(names)).existsSync()) return E.notFound;
        _cwds[_peerKey] = names;
        onLog('${peerName ?? 'PC-1500'} is in /${names.join('/')}');
        return 0;
      case Op.mkdir:
        final names = _resolve(name);
        if (names.isEmpty) return E.exists;
        final path = _hostPath(names);
        if (FileSystemEntity.typeSync(path) != FileSystemEntityType.notFound) return E.exists;
        if (!Directory(path).parent.existsSync()) return E.notFound;
        Directory(path).createSync();
        onLog('Made /${names.join('/')}');
        return 0;
      case Op.rmdir:
        final names = _resolve(name);
        final dir = Directory(_hostPath(names));
        if (names.isEmpty || !dir.existsSync()) return E.notFound;
        if (dir.listSync().isNotEmpty) return E.io;
        dir.deleteSync();
        onLog('Removed /${names.join('/')}');
        return 0;
      case Op.rm:
        final names = _resolve(name);
        final file = File(_hostPath(names));
        if (names.isEmpty || !file.existsSync()) return E.notFound;
        file.deleteSync();
        onLog('Deleted /${names.join('/')}');
        return 0;
      case Op.cp:
      case Op.mv:
        final from = _resolve(name);
        final fromPath = _hostPath(from);
        final type = FileSystemEntity.typeSync(fromPath);
        if (from.isEmpty ||
            type == FileSystemEntityType.notFound ||
            (op == Op.cp && type != FileSystemEntityType.file)) {
          return E.notFound;
        }
        var to = _resolve(name2);
        if (Directory(_hostPath(to)).existsSync()) to = [...to, from.last]; // into it
        final toPath = _hostPath(to);
        if (to.isEmpty || toPath == fromPath) return E.exists;
        final there = FileSystemEntity.typeSync(toPath);
        if (there == FileSystemEntityType.directory) return E.exists;
        if (there != FileSystemEntityType.notFound && !overwrite) return E.exists;
        if (!Directory(toPath).parent.existsSync()) return E.notFound;
        if (there != FileSystemEntityType.notFound) File(toPath).deleteSync();
        if (op == Op.cp) {
          File(fromPath).copySync(toPath);
        } else if (type == FileSystemEntityType.directory) {
          Directory(fromPath).renameSync(toPath);
        } else {
          File(fromPath).renameSync(toPath);
        }
        onLog('${op == Op.cp ? 'Copied' : 'Moved'} /${from.join('/')} to /${to.join('/')}');
        return 0;
      default:
        return E.unsupported;
    }
  }

  /// ACK (with a pairing answer's payload) or ERR.
  Future<void> _answer(int seq, [int error = 0, List<int> payload = const []]) =>
      _send(_frame(error == 0 ? T.ack : T.err, seq, error == 0 ? payload : [error]));

  /// Sealed once the link has authenticated.
  Future<void> _send(Uint8List frame) {
    final s = _session;
    if (s == null) return send(frame);
    final sealed = s.seal(frame);
    return sealed == null ? Future.value() : send(sealed);
  }

  /// A frame out and its answer: 0 for ACK, else the ERR code (or
  /// E.aborted if none came -- sec.4).
  Future<int> _request(int type, List<int> payload) async {
    final seq = _txSeq;
    _txSeq = (_txSeq + 1) & 0xFF;
    _pendingSeq = seq;
    _pending = Completer<int>();
    await _send(_frame(type, seq, payload));
    return _pending!.future.timeout(answerTimeout, onTimeout: () {
      onLog('No answer from the PC-1500');
      return E.aborted;
    });
  }

  static Uint8List _frame(int type, int seq, List<int> payload) =>
      Uint8List.fromList([type, seq, payload.length & 0xFF, payload.length >> 8, ...payload]);
}
